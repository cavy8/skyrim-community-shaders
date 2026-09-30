#include "StrandRenderer.h"

#include <algorithm>
#include <bit>
#include <sstream>

#include "Deferred.h"
#include "Features/ReverseZ.h"
#include "MeshExtract.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

namespace Strands
{
	namespace
	{
		constexpr uint32_t kMaxRunningJobs = 2;
		constexpr uint32_t kInstanceEvictFrames = 300;  // unseen this long: free its buffers
		constexpr uint32_t kAssetEvictFrames = 1800;    // unused this long: free the asset
		constexpr uint32_t kMinDrawnStrands = 64;
		constexpr float kLodHysteresis = 1.05f;
		constexpr float kHiddenViewportOrigin = 30000.0f;  // past any render target, even scaled by dynamic resolution
		constexpr float kHiddenViewportSize = 1024.0f;     // big enough to read the dynamic resolution scale back from

		// Simulation.
		constexpr uint32_t kMaxSimGapFrames = 2;    // unsimulated longer than this: restart from the targets
		constexpr float kMaxFrameTime = 0.05f;      // TressFX's clamp: longer frames slow the hair rather than destabilise it
		constexpr float kSimStep = 1.0f / 60.0f;    // TressFX's settings are per step, and its samples ran at 60 Hz
		constexpr uint32_t kMaxSimSteps = 4;        // kMaxFrameTime and the remainder of a step
		constexpr float kSimFadeStart = 0.75f;      // of the physics distance
		constexpr float kTeleportDistance = 40.0f;  // a root moving further in one frame restarts its strand,
		constexpr float kTeleportSpeed = 4000.0f;   // or, in a long frame, moving faster than this (units/s)
		constexpr float kWindMagnitude = 125.0f;    // TressFX's wind magnitude at the weather's full wind speed
		constexpr float kWindConeAngle = 40.0f;     // degrees between TressFX's four wind vectors and the wind
		// Body colliders, radii at scale 1: well inside a body, so hair resting on it stays put.
		constexpr float kNeckRadius = 3.0f;
		constexpr float kChestRadius = 5.5f;
		constexpr float kBackRadius = 6.5f;
		constexpr float kShoulderRadius = 3.5f;
		constexpr float kArmRadius = 3.0f;
		// Head field (the actor's head mesh as a radial height field, see BuildHeadField).
		constexpr uint32_t kHeadFieldSize = 64;           // HAIR_STRANDS_HEAD_FIELD_SIZE: texels a side, about 3 degrees each
		constexpr float kHeadFieldSampleSpacing = 0.2f;   // units between the surface points splatted into it
		constexpr float kHeadFieldMinHeadWeight = 0.5f;   // vertices less on the head bone (the neck) are left to the neck capsule
		constexpr float kHeadFieldMaxRadius = 30.0f;      // units; anything further out is not the head
		constexpr uint32_t kHeadFieldFillPasses = 3;      // closes eye and mouth holes; the neck opening stays empty
		constexpr float kHeadFieldMinCoverage = 0.5f;     // of the directions; less and the head sphere is used
		constexpr float kHeadFieldMaxRadiusRatio = 1.5f;  // of the median radius: the nose, not a muzzle or ear tips
		constexpr float kHeadFieldMinMedianRatio = 0.7f;  // median radius over the head sphere's: a head round the same centre
		constexpr float kHeadFieldMaxMedianRatio = 2.0f;
		constexpr uint32_t kBodyCheckFrames = 15;  // frames between looks at what an actor wears (body colliders)

		const wchar_t* kLightingShaderPath = L"Data\\Shaders\\HairStrands\\StrandLighting.hlsl";
		const wchar_t* kSkinShaderPath = L"Data\\Shaders\\HairStrands\\StrandSkin.cs.hlsl";
		const wchar_t* kSimShaderPath = L"Data\\Shaders\\HairStrands\\StrandSim.cs.hlsl";
		const char* kSkinShaderFailure = "Strand skinning shader failed to compile; strands are off";
		const char* kSimShaderFailure = "Strand simulation shader failed to compile; strands are not simulated";

		using LightingFlags = SIE::ShaderCache::LightingShaderFlags;
		using UtilityFlags = SIE::ShaderCache::UtilityShaderFlags;

		std::string ToLower(std::string_view a_text)
		{
			std::string result{ a_text };
			std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		// --- Hair classification ---

		struct HeadPartMatch
		{
			bool underFace = false;                   // the geometry hangs under the face node: a head part
			const RE::BGSHeadPart* part = nullptr;    // the head part it belongs to, if the NPC record lists it
			const RE::BGSHeadPart* parent = nullptr;  // the part listing it as an extra part, if any
		};

		// The head part named a_partName among a_parts, or among their extra parts.
		bool MatchHeadPart(RE::BGSHeadPart** a_parts, uint32_t a_count, const RE::BSFixedString& a_partName, HeadPartMatch& o_match)
		{
			if (!a_parts)
				return false;
			for (uint32_t i = 0; i < a_count; ++i) {
				const auto* part = a_parts[i];
				if (!part)
					continue;
				if (part->formEditorID == a_partName) {
					o_match.part = part;
					return true;
				}
				for (const auto* extra : part->extraParts) {
					if (extra && extra->formEditorID == a_partName) {
						o_match.part = extra;
						o_match.parent = part;
						return true;
					}
				}
			}
			return false;
		}

		// Head parts hang under the actor's skinned face node, each as a child named by the
		// part's editor ID; the geometry's ancestor right under that node names its part.
		HeadPartMatch FindHeadPart(RE::Actor* a_actor, const RE::BSGeometry* a_geometry)
		{
			HeadPartMatch match;
			const auto* faceNode = a_actor->GetFaceNodeSkinned();
			if (!faceNode)
				return match;
			const RE::NiAVObject* partRoot = a_geometry;
			while (partRoot && partRoot->parent != faceNode)
				partRoot = partRoot->parent;
			if (!partRoot)
				return match;
			match.underFace = true;
			auto* npc = a_actor->GetActorBase();
			if (!npc)
				return match;
			if (npc->HasOverlays() && MatchHeadPart(npc->GetBaseOverlays(), npc->GetNumBaseOverlays(), partRoot->name, match))
				return match;
			MatchHeadPart(npc->headParts, static_cast<uint32_t>(std::max<std::int8_t>(npc->numHeadParts, 0)), partRoot->name, match);
			return match;
		}

		bool HasAlpha(const RE::BSGeometry* a_geometry)
		{
			const auto& property = a_geometry->GetGeometryRuntimeData().alphaProperty;
			if (!property || property->GetRTTI() != globals::rtti::NiAlphaPropertyRTTI.get())
				return false;
			const auto* alpha = static_cast<const RE::NiAlphaProperty*>(property.get());
			return alpha->GetAlphaTesting() || alpha->GetAlphaBlending();
		}

		// The hair-tint material: what the HAIR technique, and so every hair shading feature,
		// treats as hair. Hair worn as equipment (wigs) has only this to go by.
		bool IsHairTintShader(const RE::BSRenderPass* a_pass)
		{
			if (!a_pass->shaderProperty || a_pass->shaderProperty->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
				return false;
			const auto* lightingProperty = static_cast<const RE::BSLightingShaderProperty*>(a_pass->shaderProperty);
			return (lightingProperty->material && lightingProperty->material->GetFeature() == RE::BSShaderMaterial::Feature::kHairTint) ||
			       lightingProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kHairTint);
		}

		// Splits ShaderCache::GetDefinesString ("NAME=VALUE NAME ...") into macro pairs.
		std::shared_ptr<std::vector<std::pair<std::string, std::string>>> ParseDefines(const std::string& a_defines)
		{
			auto result = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
			std::istringstream stream(a_defines);
			std::string token;
			while (stream >> token) {
				const auto equals = token.find('=');
				if (equals == std::string::npos)
					result->emplace_back(token, "");
				else
					result->emplace_back(token.substr(0, equals), token.substr(equals + 1));
			}
			return result;
		}

		D3D11_BUFFER_DESC StructuredDesc(uint32_t a_stride, uint32_t a_count, D3D11_USAGE a_usage, UINT a_bind, UINT a_cpu)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = a_stride * a_count;
			desc.Usage = a_usage;
			desc.BindFlags = a_bind;
			desc.CPUAccessFlags = a_cpu;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = a_stride;
			return desc;
		}

		D3D11_SHADER_RESOURCE_VIEW_DESC BufferSRVDesc(uint32_t a_count)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			desc.Buffer.FirstElement = 0;
			desc.Buffer.NumElements = a_count;
			return desc;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC BufferUAVDesc(uint32_t a_count)
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			desc.Buffer.FirstElement = 0;
			desc.Buffer.NumElements = a_count;
			return desc;
		}

		float Smoothstep(float a_edge0, float a_edge1, float a_x)
		{
			const float t = std::clamp((a_x - a_edge0) / std::max(a_edge1 - a_edge0, 1e-4f), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		}

		float3 ToFloat3(const RE::NiPoint3& a_point)
		{
			return { a_point.x, a_point.y, a_point.z };
		}

		// The rendered frame, counted at Present: the same for a hair's depth prepass and its
		// lighting pass, whichever order the feature's per-frame hooks run in.
		uint32_t RenderFrame()
		{
			return globals::state->frameCount;
		}

		// The strands' colour texture, mips included; null if it cannot be created.
		winrt::com_ptr<ID3D11ShaderResourceView> CreateColourTexture(const StrandColourImage& a_image, uint64_t& o_bytes)
		{
			o_bytes = 0;
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_image.width;
			desc.Height = a_image.height;
			desc.MipLevels = static_cast<UINT>(a_image.mips.size());
			desc.ArraySize = 1;
			desc.Format = a_image.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_IMMUTABLE;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			std::vector<D3D11_SUBRESOURCE_DATA> init(a_image.mips.size());
			for (size_t k = 0; k < init.size(); ++k) {
				init[k] = { a_image.mips[k].data(), std::max(a_image.width >> k, 1u) * 4u, 0 };
				o_bytes += a_image.mips[k].size() * sizeof(uint32_t);
			}
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			if (FAILED(globals::d3d::device->CreateTexture2D(&desc, init.data(), texture.put())) ||
				FAILED(globals::d3d::device->CreateShaderResourceView(texture.get(), nullptr, srv.put()))) {
				o_bytes = 0;
				return nullptr;
			}
			Util::SetResourceName(texture.get(), "HairStrands::StrandColour");
			Util::SetResourceName(srv.get(), "HairStrands::StrandColour SRV");
			return srv;
		}

		// Octahedral map of a unit direction to [0, 1]^2, as HairStrandsSkin::HeadFieldUV.
		float2 OctahedralUV(float3 a_direction)
		{
			a_direction /= std::max(std::abs(a_direction.x) + std::abs(a_direction.y) + std::abs(a_direction.z), 1e-8f);
			float2 p{ a_direction.x, a_direction.y };
			if (a_direction.z < 0.0f)
				p = { (1.0f - std::abs(a_direction.y)) * (a_direction.x >= 0.0f ? 1.0f : -1.0f), (1.0f - std::abs(a_direction.x)) * (a_direction.y >= 0.0f ? 1.0f : -1.0f) };
			return { p.x * 0.5f + 0.5f, p.y * 0.5f + 0.5f };
		}

		int32_t FindBone(const std::vector<std::string>& a_names, std::string_view a_fragment)
		{
			for (size_t b = 0; b < a_names.size(); ++b) {
				if (ToLower(a_names[b]).find(a_fragment) != std::string::npos)
					return static_cast<int32_t>(b);
			}
			return -1;
		}

		// The actor's head mesh: the geometry of its Face head part, a child of the face node
		// named by the part's editor ID (or the first skinned geometry under that child).
		RE::BSGeometry* FindFaceGeometry(RE::Actor* a_actor)
		{
			auto* faceNode = a_actor->GetFaceNodeSkinned();
			auto* npc = a_actor->GetActorBase();
			if (!faceNode || !npc)
				return nullptr;
			const auto find = [&](RE::BGSHeadPart** a_parts, uint32_t a_count) -> RE::BSGeometry* {
				for (uint32_t i = 0; a_parts && i < a_count; ++i) {
					const auto* part = a_parts[i];
					if (!part || part->type != RE::BGSHeadPart::HeadPartType::kFace)
						continue;
					auto* object = faceNode->GetObjectByName(part->formEditorID);
					if (auto* geometry = object ? object->AsGeometry() : nullptr)
						return geometry;
					if (auto* node = object ? object->AsNode() : nullptr) {
						for (auto& child : node->GetChildren()) {
							auto* geometry = child ? child->AsGeometry() : nullptr;
							if (geometry && geometry->GetGeometryRuntimeData().skinInstance)
								return geometry;
						}
					}
				}
				return nullptr;
			};
			if (npc->HasOverlays()) {
				if (auto* geometry = find(npc->GetBaseOverlays(), npc->GetNumBaseOverlays()))
					return geometry;
			}
			return find(npc->headParts, static_cast<uint32_t>(std::max<std::int8_t>(npc->numHeadParts, 0)));
		}

		// True if a_target is a_node or hangs under it within a_depth levels. a_target is only
		// compared, never read.
		bool Contains(RE::NiAVObject* a_node, const RE::NiAVObject* a_target, int a_depth)
		{
			if (!a_node)
				return false;
			if (a_node == a_target)
				return true;
			auto* node = a_depth > 0 ? a_node->AsNode() : nullptr;
			if (!node)
				return false;
			for (auto& child : node->GetChildren()) {
				if (Contains(child.get(), a_target, a_depth - 1))
					return true;
			}
			return false;
		}

		// The skin instance of a layer's twin, drawn from the layer's pass. The twin's geometry is
		// read only once it is found under the same actor's face node, which proves it alive.
		RE::NiSkinInstance* LiveTwinSkin(RE::BSGeometry* a_layerGeometry, RE::BSGeometry* a_twinGeometry, const RE::NiSkinInstance* a_twinSkin, uint32_t a_twinVertexCount)
		{
			auto* userData = a_layerGeometry->GetUserData();
			auto* actor = userData ? userData->As<RE::Actor>() : nullptr;
			auto* faceNode = actor ? actor->GetFaceNodeSkinned() : nullptr;
			if (!faceNode || !Contains(faceNode, a_twinGeometry, 3))
				return nullptr;
			auto* skin = a_twinGeometry->GetGeometryRuntimeData().skinInstance.get();
			if (!skin || skin != a_twinSkin || !skin->skinPartition || !skin->skinData || skin->skinPartition->vertexCount != a_twinVertexCount)
				return nullptr;
			return skin;
		}
	}

	struct StrandRenderer::Asset
	{
		enum class State
		{
			Readback,  // waiting for the texture's alpha (and the flow map) to reach the CPU
			Queued,
			Running,
			Ready,
			Failed
		};

		std::string key;
		uint32_t serial = 0;  // tells a replaced asset from its successor
		State state = State::Queued;
		std::string error;
		HairMeshData mesh;              // released once the job starts
		CoverageReadback readback;      // decoded into mesh.coverage and colourImage by the job
		CoverageReadback flowReadback;  // decoded into mesh.flow by the job
		StrandColourImage colourImage;  // taken when the job finishes
		StrandStyle style;
		std::future<std::unique_ptr<StrandAssetData>> job;

		uint32_t strandCount = 0;
		uint32_t pointsPerStrand = 0;
		float averageLength = 0.0f;
		SeedMode seedingUsed = SeedMode::Roots;
		uint32_t guideCount = 0;
		int32_t headBone = -1;
		float3 headCentre;
		float headRadius = 0.0f;
		std::unique_ptr<Buffer> restPoints;
		std::unique_ptr<Buffer> strandInfo;
		winrt::com_ptr<ID3D11ShaderResourceView> colour;  // the strands' colour texture; null: the card texture
		uint64_t colourBytes = 0;
		uint32_t lastUsedFrame = 0;

		uint64_t GpuBytes() const { return static_cast<uint64_t>(strandCount) * (pointsPerStrand * sizeof(RestPoint) + sizeof(StrandInfo)) + colourBytes; }
	};

	struct StrandRenderer::Instance
	{
		// Validation of the cached geometry pointer (never dereferenced outside the draw hooks).
		RE::NiSkinInstance* skinInstance = nullptr;
		uint32_t vertexCount = 0;

		HairKey key;
		RE::FormID actorId = 0;
		bool isHair = false;   // drawn as strands when converted
		bool layer = false;    // a card layer over hair drawn as strands: hidden while its twin has strands
		bool blended = false;  // alpha-blended cards: drawn forward, after the deferred passes, with no depth prepass
		bool isPlayer = false;
		bool converted = false;
		bool authored = false;
		bool edited = false;
		std::string source;
		StrandStyle style;
		uint32_t styleGeneration = UINT32_MAX;

		std::shared_ptr<Asset> asset;      // drawn
		std::shared_ptr<Asset> nextAsset;  // generating for a changed style

		std::unique_ptr<Buffer> skinned;
		uint32_t skinnedCapacity = 0;
		std::unique_ptr<Buffer> guideState;  // simulated guide points, kept between frames
		uint32_t guideCapacity = 0;
		std::unique_ptr<Buffer> palette;
		uint32_t paletteBones = 0;
		std::vector<float4> paletteData;
		std::vector<float4> previousAbsolute;  // last frame's palette, absolute translations
		uint32_t previousFrame = 0;            // RenderFrame() of previousAbsolute

		uint32_t lastSeenFrame = 0;
		uint32_t lastSkinnedFrame = UINT32_MAX;  // RenderFrame() of the last skinning
		float distance = 0.0f;
		bool allowed = false;
		float budgetScale = 1.0f;
		bool lodActive = false;
		uint32_t strandDescriptor = 0;           // lighting permutation whose strand shaders drew this hair last
		uint32_t lastPrepassFrame = UINT32_MAX;  // RenderFrame() in which the depth prepass drew the strands
		uint32_t layerDrawFrame = UINT32_MAX;    // RenderFrame() in which its alpha-tested layer drew the strands

		// This frame's draw parameters.
		bool drawThisFrame = false;
		uint32_t activeStrands = 0;
		uint32_t subdivisions = 1;
		float widthScale = 1.0f;
		float minWidthPerDistance = 0.0f;
		float3 skinEye;
		float3 skinPreviousEye;

		// Simulation.
		float simWeight = 0.0f;                 // 1 up close, fading to 0 (plain skinning) at the physics distance
		float3 simEye;                          // camera the stored guide state is relative to
		uint32_t lastSimFrame = UINT32_MAX;     // RenderFrame() of the last simulation; UINT32_MAX: restart
		uint32_t simAssetSerial = 0;            // the asset the stored guide state belongs to
		std::unique_ptr<Buffer> headField;      // the actor's head surface; null: the head sphere
		uint32_t headFieldSerial = UINT32_MAX;  // the asset the head field was built (or tried) for

		// Body colliders from the actor's worn meshes (BodyField.h); null: the bone capsules.
		std::unique_ptr<Buffer> bodyField;
		std::array<BodyColliderShape, kBodySlots> bodyShapes{};
		uint64_t bodySignature = 0;                           // the worn meshes they were built (or tried) from
		uint32_t bodyCheckFrame = UINT32_MAX;                 // RenderFrame() of the last look at the worn meshes
		std::future<std::unique_ptr<BodyFieldData>> bodyJob;  // a build in flight
		uint64_t bodyJobSignature = 0;                        // the worn meshes it builds from
		size_t bodyJobMeshes = 0;
		std::array<std::array<float4, 3>, kBodySlots> previousBodyFrames{};  // last frame's field-to-world rows, absolute translations
		uint32_t previousBodyFrame = UINT32_MAX;                             // RenderFrame() of previousBodyFrames
	};

	struct StrandRenderer::ShaderVariant
	{
		std::atomic<bool> vsDone{ false };
		std::atomic<bool> psDone{ false };
		winrt::com_ptr<ID3D11VertexShader> vs;
		winrt::com_ptr<ID3D11PixelShader> ps;

		bool Ready() const { return vsDone && psDone && vs && ps; }
		bool Failed() const { return vsDone && psDone && (!vs || !ps); }
	};

	StrandRenderer::StrandRenderer(StyleLibrary& a_library) :
		library(a_library)
	{}

	StrandRenderer::~StrandRenderer()
	{
		for (auto& [key, asset] : assets) {
			if (asset->job.valid())
				asset->job.wait();
		}
	}

	void StrandRenderer::Reset()
	{
		instances.clear();
		for (auto& [key, asset] : assets) {
			if (asset->job.valid())
				asset->job.wait();
		}
		assets.clear();
		jobQueue.clear();
		currentPass = nullptr;
		currentInstance = nullptr;
		stats = {};
	}

	void StrandRenderer::ForgetInstances()
	{
		instances.clear();
		currentPass = nullptr;
		currentInstance = nullptr;
	}

	void StrandRenderer::ClearShaders()
	{
		{
			std::scoped_lock lock(variantMutex);
			// A compile still running holds its variant through the callback's shared state;
			// dropping the map entry only stops new draws from using it.
			variants.clear();
		}
		std::scoped_lock lock(computeShaderMutex);
		for (auto* slot : { &skinShader, &simShader }) {
			slot->shader = nullptr;
			slot->requested = false;
			slot->failed = false;
			++slot->generation;
		}
	}

	void StrandRenderer::InvalidateStyles()
	{
		for (auto& [geometry, instance] : instances)
			instance->styleGeneration = UINT32_MAX;
	}

	void StrandRenderer::SetStyleOverride(const HairKey& a_key, std::optional<StrandStyle> a_style)
	{
		if (a_style)
			overrides[a_key.ToString()] = *a_style;
		else
			overrides.erase(a_key.ToString());
		InvalidateStyles();
	}

	std::optional<StrandStyle> StrandRenderer::GetStyleOverride(const HairKey& a_key) const
	{
		auto it = overrides.find(a_key.ToString());
		return it != overrides.end() ? std::optional<StrandStyle>{ it->second } : std::nullopt;
	}

	std::vector<InstanceView> StrandRenderer::GetInstances() const
	{
		std::vector<InstanceView> result;
		for (const auto& [geometry, instance] : instances) {
			if (!instance->isHair || frame - instance->lastSeenFrame > 2)
				continue;
			InstanceView view;
			view.key = instance->key;
			view.style = instance->style;
			view.converted = instance->converted;
			view.authored = instance->authored;
			view.edited = instance->edited;
			view.isPlayer = instance->isPlayer;
			view.source = instance->authored ? instance->source : std::string{};
			view.disabledByStyle = instance->isHair && !instance->style.enabled;
			view.distance = instance->distance;
			view.activeStrands = instance->drawThisFrame ? instance->activeStrands : 0;
			view.subdivisions = instance->subdivisions;
			const auto& shown = instance->nextAsset ? instance->nextAsset : instance->asset;
			using Status = InstanceView::Status;
			if (!instance->converted)
				view.status = Status::Cards;
			else if (!shown)
				view.status = Status::Waiting;
			else if (shown->state == Asset::State::Failed) {
				view.status = Status::Failed;
				view.error = shown->error;
			} else if (shown->state != Asset::State::Ready)
				view.status = Status::Generating;
			else
				view.status = Status::Ready;
			if (instance->asset && instance->asset->state == Asset::State::Ready) {
				view.strands = instance->asset->strandCount;
				view.pointsPerStrand = instance->asset->pointsPerStrand;
			}
			// One row per distinct hair: prefer the player's, then the nearest.
			auto same = std::ranges::find_if(result, [&](const InstanceView& v) { return v.key == view.key; });
			if (same == result.end())
				result.push_back(std::move(view));
			else if (view.isPlayer || (!same->isPlayer && view.distance < same->distance))
				*same = std::move(view);
		}
		std::ranges::sort(result, [](const InstanceView& a, const InstanceView& b) {
			return a.isPlayer != b.isPlayer ? a.isPlayer : a.distance < b.distance;
		});
		return result;
	}

	void StrandRenderer::BeginFrame(const RenderSettings& a_settings)
	{
		settings = a_settings;
		++frame;
		currentPass = nullptr;
		currentInstance = nullptr;
		currentVariant = nullptr;
		currentDepthOnly = false;

		// Simulation clock, once per frame: fixed steps shared by all hair, so every hair's steps end
		// at the same times. Paused, no step runs and the hair holds still.
		const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
		frameDeltaTime = paused ? 0.0f : std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, kMaxFrameTime);
		const float elapsed = simAccumulator + frameDeltaTime;
		frameSteps = std::min(static_cast<uint32_t>(elapsed / kSimStep + 1e-4f), kMaxSimSteps);
		firstStepFraction = frameDeltaTime > 0.0f ? (kSimStep - simAccumulator) / frameDeltaTime : 0.0f;
		stepFraction = frameDeltaTime > 0.0f ? kSimStep / frameDeltaTime : 0.0f;
		simAccumulator = std::clamp(elapsed - frameSteps * kSimStep, 0.0f, kSimStep * 0.999f);
		displayAlpha = simAccumulator / kSimStep;
		simulationStep += frameSteps;

		// The weather's wind, as TressFX's SetWind makes it: the magnitude swells and fades with
		// sin^2(frame x 0.01) + 0.5, and four vectors 40 degrees off the wind's direction form a cone
		// that each strand mixes its own wind from. Weather wind is level, so the turn from TressFX's
		// X axis to it is a turn about Z.
		windCorners = {};
		if (auto* sky = globals::game::sky; sky && settings.physics && !Util::IsInterior()) {
			const double swell = std::sin(static_cast<double>(simulationStep) * 0.01);
			const float magnitude = std::clamp(sky->windSpeed, 0.0f, 1.0f) * kWindMagnitude * settings.windStrength * static_cast<float>(swell * swell + 0.5);
			const float cone = DirectX::XMConvertToRadians(kWindConeAngle);
			const float c = std::cos(cone), s = std::sin(cone);
			// TressFX's corners: X turned about +Y, -Y, +Z and -Z.
			const float3 corners[4] = { { c, 0.0f, -s }, { c, 0.0f, s }, { c, s, 0.0f }, { c, -s, 0.0f } };
			const float heading = DirectX::XM_PIDIV2 - sky->windAngle;  // the wind blows along (sin angle, cos angle)
			const float ch = std::cos(heading), sh = std::sin(heading);
			for (size_t k = 0; k < std::size(corners); ++k) {
				const float3& v = corners[k];
				windCorners[k] = { (v.x * ch - v.y * sh) * magnitude, (v.x * sh + v.y * ch) * magnitude, v.z * magnitude, 0.0f };
			}
		}

		if (library.GetGeneration() != libraryGeneration) {
			libraryGeneration = library.GetGeneration();
			InvalidateStyles();
		}

		// Textures that reached the CPU: their hair can be generated now.
		for (auto& [key, asset] : assets) {
			if (asset->state != Asset::State::Readback)
				continue;
			// A readback is in flight while it has a staging texture.
			const auto poll = [&](CoverageReadback& a_readback, std::string_view a_failure) {
				if (!a_readback.staging)
					return false;
				const auto status = PollCoverageReadback(a_readback);
				if (status == ReadbackStatus::Failed)
					logger::warn("[HairStrands] {}: {}", asset->key, a_failure);
				return status == ReadbackStatus::Pending;
			};
			const bool texturePending = poll(asset->readback, "could not read the hair texture back; strands fill the whole cards");
			const bool flowPending = poll(asset->flowReadback, "could not read the flow map back; flow follows the texture");
			if (texturePending || flowPending)
				continue;
			asset->state = Asset::State::Queued;
			jobQueue.push_back(asset);
		}

		// Start queued generation jobs, a few at a time, and upload finished ones.
		uint32_t running = 0;
		for (auto& [key, asset] : assets) {
			if (asset->state != Asset::State::Running)
				continue;
			if (asset->job.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				++running;
				continue;
			}
			auto data = asset->job.get();
			const StrandColourImage colourImage = std::move(asset->colourImage);
			asset->colourImage = {};
			if (!data) {
				asset->state = Asset::State::Failed;
				continue;
			}
			asset->strandCount = data->StrandCount();
			asset->pointsPerStrand = data->pointsPerStrand;
			asset->averageLength = data->averageLength;
			asset->seedingUsed = data->seedingUsed;
			asset->guideCount = data->guideCount;
			asset->headBone = data->headBone;
			asset->headCentre = data->headCentre;
			asset->headRadius = data->headRadius;
			D3D11_SUBRESOURCE_DATA pointsInit{ data->points.data(), 0, 0 };
			D3D11_SUBRESOURCE_DATA strandsInit{ data->strands.data(), 0, 0 };
			try {
				asset->restPoints = std::make_unique<Buffer>(StructuredDesc(sizeof(RestPoint), static_cast<uint32_t>(data->points.size()), D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0), &pointsInit, "HairStrands::RestPoints");
				asset->restPoints->CreateSRV(BufferSRVDesc(static_cast<uint32_t>(data->points.size())));
				asset->strandInfo = std::make_unique<Buffer>(StructuredDesc(sizeof(StrandInfo), asset->strandCount, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0), &strandsInit, "HairStrands::StrandInfo");
				asset->strandInfo->CreateSRV(BufferSRVDesc(asset->strandCount));
				if (!colourImage.Empty())
					asset->colour = CreateColourTexture(colourImage, asset->colourBytes);
				asset->state = Asset::State::Ready;
				logger::info("[HairStrands] {}: {} strands x {} points (avg length {:.1f}, {} seeding), {} guides, head collider radius {:.1f}, {}{}", asset->key, asset->strandCount,
					asset->pointsPerStrand, asset->averageLength, asset->seedingUsed == SeedMode::Area ? "area" : "root", asset->guideCount, asset->headRadius,
					asset->colour ? std::format("{}x{} strand colour", colourImage.width, colourImage.height) : std::string("card colour"),
					data->flowMapShare > 0.0f ? std::format(", flow map on {:.0f}% of the hair", data->flowMapShare * 100.0f) : std::string());
			} catch (const std::exception& e) {
				asset->state = Asset::State::Failed;
				asset->error = "GPU upload failed";
				logger::error("[HairStrands] {}: {}", asset->key, e.what());
			}
		}
		std::erase_if(jobQueue, [](const auto& a) { return a->state != Asset::State::Queued; });
		while (running < kMaxRunningJobs && !jobQueue.empty()) {
			auto asset = jobQueue.front();
			jobQueue.erase(jobQueue.begin());
			asset->state = Asset::State::Running;
			// Raw pointer: the task is stored in the future the asset owns. A running asset is
			// never evicted, and Reset() waits for every job before freeing assets.
			asset->job = std::async(std::launch::async, [asset = asset.get()]() -> std::unique_ptr<StrandAssetData> {
				auto data = std::make_unique<StrandAssetData>();
				std::string error;
				bool ok = false;
				try {
					if (!asset->readback.bytes.empty()) {
						std::string coverageError;
						if (!DecodeCoverage(asset->readback, asset->mesh.coverage, asset->colourImage, coverageError))
							logger::warn("[HairStrands] {}: {}; strands fill the whole cards", asset->key, coverageError);
						asset->readback = {};
					}
					if (!asset->flowReadback.bytes.empty()) {
						std::string flowError;
						if (!DecodeFlow(asset->flowReadback, asset->mesh.flow, flowError))
							logger::warn("[HairStrands] {}: flow map: {}; flow follows the texture", asset->key, flowError);
						asset->flowReadback = {};
					}
					ok = GenerateStrands(asset->mesh, asset->style, *data, error);
				} catch (const std::exception& e) {
					error = e.what();  // bad_alloc on an absurd mesh must not reach the render thread
				}
				asset->mesh = {};
				if (!ok) {
					asset->error = error;
					logger::warn("[HairStrands] {}: not converted: {}", asset->key, error);
					return nullptr;
				}
				return data;
			});
			++running;
		}

		// Forget hair not drawn for a while (actors unloaded), and assets nobody uses.
		std::erase_if(instances, [&](const auto& item) { return frame - item.second->lastSeenFrame > kInstanceEvictFrames; });
		for (const auto& [geometry, instance] : instances) {
			for (const auto* asset : { instance->asset.get(), instance->nextAsset.get() }) {
				if (asset)
					const_cast<Asset*>(asset)->lastUsedFrame = frame;
			}
		}
		std::erase_if(assets, [&](const auto& item) {
			const auto& asset = item.second;
			return asset->state != Asset::State::Running && asset->state != Asset::State::Queued && frame - asset->lastUsedFrame > kAssetEvictFrames;
		});

		// Budget: the player first, then the nearest converted hair seen last frame.
		std::vector<Instance*> candidates;
		for (auto& [geometry, instance] : instances) {
			instance->allowed = false;
			if (instance->converted && instance->asset && instance->asset->state == Asset::State::Ready && frame - instance->lastSeenFrame <= 2)
				candidates.push_back(instance.get());
		}
		std::ranges::sort(candidates, [](const Instance* a, const Instance* b) {
			return a->isPlayer != b->isPlayer ? a->isPlayer : a->distance < b->distance;
		});
		uint64_t budget = settings.maxStrandsPerFrame;
		uint32_t actors = 0;
		for (auto* instance : candidates) {
			if (settings.playerOnly && !instance->isPlayer)
				continue;
			if (actors >= settings.maxActors || budget < kMinDrawnStrands)
				break;
			const uint64_t wanted = instance->lodActive ? std::max<uint32_t>(instance->activeStrands, kMinDrawnStrands) : instance->asset->strandCount;
			instance->allowed = true;
			instance->budgetScale = wanted > budget ? static_cast<float>(budget) / wanted : 1.0f;
			budget -= std::min<uint64_t>(budget, wanted);
			++actors;
		}

		// Statistics for the previous frame.
		stats = {};
		for (const auto& [geometry, instance] : instances) {
			if (!instance->isHair || frame - instance->lastSeenFrame > 2)
				continue;
			++stats.trackedHair;
			stats.convertedHair += instance->converted ? 1 : 0;
			stats.gpuBytes += static_cast<uint64_t>(instance->skinnedCapacity) * sizeof(SkinnedPoint) + static_cast<uint64_t>(instance->guideCapacity) * sizeof(GuidePoint);
		}
		for (const auto& [key, asset] : assets) {
			++stats.assets;
			stats.pendingJobs += (asset->state == Asset::State::Readback || asset->state == Asset::State::Queued || asset->state == Asset::State::Running) ? 1 : 0;
			stats.gpuBytes += asset->state == Asset::State::Ready ? asset->GpuBytes() : 0;
		}
		stats.strandsDrawn = strandsThisFrame;
		stats.drawnHair = drawnThisFrame;
		stats.simulatedHair = simulatedThisFrame;
		stats.guidesSimulated = guidesThisFrame;
		strandsThisFrame = 0;
		drawnThisFrame = 0;
		simulatedThisFrame = 0;
		guidesThisFrame = 0;
	}

	void StrandRenderer::Classify(Instance& a_instance, RE::BSRenderPass* a_pass, RE::BSGeometry* a_geometry)
	{
		a_instance = {};
		auto& geometryData = a_geometry->GetGeometryRuntimeData();
		a_instance.skinInstance = geometryData.skinInstance.get();
		a_instance.vertexCount = a_instance.skinInstance->skinPartition->vertexCount;

		auto* userData = a_geometry->GetUserData();
		auto* actor = userData ? userData->As<RE::Actor>() : nullptr;
		if (!actor)
			return;

		// Only hair cards: the hair-tint material with alpha. Beads, ties and other solid
		// pieces of a hair mesh keep their own look.
		if (!IsHairTintShader(a_pass) || !HasAlpha(a_geometry))
			return;
		a_instance.blended = static_cast<const RE::NiAlphaProperty*>(a_geometry->GetGeometryRuntimeData().alphaProperty.get())->GetAlphaBlending();

		// Head parts: only the Hair type becomes strands. Brows, lashes and beards are hair
		// tinted too but stay cards. The Misc extra parts of a hair are its hairline (a scalp
		// cap that stays under the strands) or a second card layer of the same mesh.
		using HeadPartType = RE::BGSHeadPart::HeadPartType;
		const auto match = FindHeadPart(actor, a_geometry);
		const RE::BGSHeadPart* part = match.part;
		if (match.underFace) {
			if (part && part->type == HeadPartType::kHair)
				a_instance.isHair = true;
			else if (part && match.parent && match.parent->type == HeadPartType::kHair)
				a_instance.layer = true;
			else
				return;
		} else {
			a_instance.isHair = true;  // hair worn as equipment
		}

		a_instance.actorId = actor->GetFormID();
		a_instance.isPlayer = actor->IsPlayerRef();
		a_instance.key.shape = a_geometry->name.c_str();
		a_instance.key.vertexCount = a_instance.vertexCount;
		uint32_t triangles = 0;
		const auto* partition = a_instance.skinInstance->skinPartition.get();
		for (uint32_t p = 0; p < partition->numPartitions; ++p)
			triangles += partition->partitions[p].triangles;
		a_instance.key.triangleCount = triangles;
		if (part) {
			a_instance.key.headPart = part->formEditorID.c_str();
			if (const char* model = part->GetModel())
				a_instance.key.model = ToLower(model);
		}

		// Hair shipped as two layers of one mesh (alpha-tested and blended copies): the first
		// seen becomes strands, the rest are hidden under it.
		if (a_instance.isHair) {
			for (const auto& [geometry, other] : instances) {
				if (other.get() != &a_instance && other->isHair && other->actorId == a_instance.actorId && other->key.vertexCount == a_instance.key.vertexCount &&
					other->key.triangleCount == a_instance.key.triangleCount && frame - other->lastSeenFrame <= 2) {
					a_instance.isHair = false;
					a_instance.layer = true;
					break;
				}
			}
		}
	}

	std::pair<RE::BSGeometry*, StrandRenderer::Instance*> StrandRenderer::FindStrandTwin(const Instance& a_layer) const
	{
		for (const auto& [geometry, other] : instances) {
			if (other->isHair && other->actorId == a_layer.actorId && other->key.vertexCount == a_layer.key.vertexCount && other->key.triangleCount == a_layer.key.triangleCount && DrawsStrands(*other))
				return { geometry, other.get() };
		}
		return { nullptr, nullptr };
	}

	bool StrandRenderer::TwinDrawsStrands(const Instance& a_layer) const
	{
		return FindStrandTwin(a_layer).second != nullptr;
	}

	bool StrandRenderer::DrawsTwin(const Instance& a_layer, const Instance& a_twin)
	{
		// KS Hairdos ship each hair as a blended mesh (alpha test 40) and an alpha-tested "Hl"
		// copy (alpha test 200). Drawn in the blended pass, the strands had no depth in the
		// prepass, so the shadow mask and every other screen-space pass built from it saw
		// whatever lay behind the hair, and they were lit forward rather than deferred.
		return !a_layer.blended && a_twin.blended && a_twin.converted && a_twin.asset && a_twin.asset->state == Asset::State::Ready;
	}

	bool StrandRenderer::DrawsStrands(const Instance& a_instance)
	{
		return a_instance.drawThisFrame && a_instance.lastSkinnedFrame != UINT32_MAX && RenderFrame() - a_instance.lastSkinnedFrame <= 1;
	}

	void StrandRenderer::ResolveStyle(Instance& a_instance)
	{
		a_instance.styleGeneration = libraryGeneration;
		const auto guess = GuessPreset(a_instance.key.headPart, a_instance.key.model, a_instance.key.shape);
		a_instance.authored = false;
		a_instance.edited = false;

		if (auto entry = library.Find(a_instance.key)) {
			a_instance.style = StyleFromJson(entry->style, guess);
			a_instance.authored = true;
			a_instance.source = entry->sourceFile;
		} else {
			a_instance.style = MakePresetStyle(guess);
			a_instance.style.preset = HairPreset::Auto;
			a_instance.source.clear();
		}
		if (auto it = overrides.find(a_instance.key.ToString()); it != overrides.end()) {
			a_instance.style = it->second;
			a_instance.edited = true;
		}
		a_instance.converted = a_instance.style.enabled && (a_instance.authored || a_instance.edited || settings.autoConvert);
	}

	std::shared_ptr<StrandRenderer::Asset> StrandRenderer::RequestAsset(Instance& a_instance, RE::BSRenderPass* a_pass, RE::BSGeometry* a_geometry)
	{
		const std::string key = std::format("{}#{:016X}", a_instance.key.ToString(), a_instance.style.GenerationHash());
		if (auto it = assets.find(key); it != assets.end())
			return it->second;

		auto asset = std::make_shared<Asset>();
		asset->key = key;
		asset->serial = ++nextAssetSerial;
		asset->style = a_instance.style;
		asset->lastUsedFrame = frame;
		std::string error;
		if (!ExtractHairMesh(a_geometry, asset->mesh, error)) {
			asset->state = Asset::State::Failed;
			asset->error = error;
			logger::warn("[HairStrands] {}: cannot read the mesh: {}", a_instance.key.ToString(), error);
		} else {
			const bool coverage = asset->style.coverageThreshold > 0.0f && BeginCoverageReadback(a_pass, asset->readback, error);
			if (asset->style.coverageThreshold > 0.0f && !coverage)
				logger::info("[HairStrands] {}: {}; strands fill the whole cards", a_instance.key.ToString(), error);
			std::string flowError;
			const bool flow = asset->style.flowAxis == FlowAxis::Auto && BeginFlowReadback(a_pass, asset->flowReadback, flowError);
			if (!flowError.empty())
				logger::warn("[HairStrands] {}: flow map: {}; flow follows the texture", a_instance.key.ToString(), flowError);
			if (coverage || flow)
				asset->state = Asset::State::Readback;
			else
				jobQueue.push_back(asset);
		}
		assets.emplace(key, asset);
		return asset;
	}

	StrandRenderer::Instance* StrandRenderer::FindOrCreateInstance(RE::BSRenderPass* a_pass, RE::BSGeometry* a_geometry)
	{
		auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
		auto& slot = instances[a_geometry];
		// A cached pointer can be a new geometry at a freed address: re-check what it skins.
		if (!slot || slot->skinInstance != skin || slot->vertexCount != skin->skinPartition->vertexCount) {
			slot = std::make_unique<Instance>();
			Classify(*slot, a_pass, a_geometry);
		}
		return slot.get();
	}

	StrandRenderer::ShaderVariant* StrandRenderer::FindVariant(uint32_t a_pixelDescriptor)
	{
		std::scoped_lock lock(variantMutex);
		auto it = variants.find(a_pixelDescriptor);
		return it != variants.end() && it->second->Ready() ? it->second.get() : nullptr;
	}

	StrandRenderer::ShaderVariant* StrandRenderer::GetVariant(uint32_t a_pixelDescriptor)
	{
		std::scoped_lock lock(variantMutex);
		if (auto it = variants.find(a_pixelDescriptor); it != variants.end())
			return it->second->Ready() ? it->second.get() : nullptr;

		auto* shader = globals::state->currentShader;
		if (!shader)
			return nullptr;

		// The strand variant is the hair's own permutation (same defines), so its pixel
		// shader matches every constant and resource the game binds for the hair.
		auto variant = std::make_shared<ShaderVariant>();
		variants.emplace(a_pixelDescriptor, variant);
		auto defines = ParseDefines(SIE::ShaderCache::GetDefinesString(*shader, a_pixelDescriptor));
		std::vector<std::pair<const char*, const char*>> macros;
		for (const auto& [name, value] : *defines)
			macros.emplace_back(name.c_str(), value.c_str());

		// The callbacks own the variant and the define strings until they run, so clearing
		// the variant map mid-compile is safe.
		globals::shaderCache->EnqueueStandaloneShaderCompile(kLightingShaderPath, "main", macros, SIE::ShaderCache::StandaloneShaderClass::Vertex,
			[variant, defines](ID3D11DeviceChild* a_shader) {
				variant->vs.attach(static_cast<ID3D11VertexShader*>(a_shader));
				variant->vsDone = true;
				if (!a_shader)
					logger::error("[HairStrands] Strand vertex shader failed to compile; this hair permutation keeps its cards");
			});
		globals::shaderCache->EnqueueStandaloneShaderCompile(kLightingShaderPath, "main", macros, SIE::ShaderCache::StandaloneShaderClass::Pixel,
			[variant, defines](ID3D11DeviceChild* a_shader) {
				variant->ps.attach(static_cast<ID3D11PixelShader*>(a_shader));
				variant->psDone = true;
				if (!a_shader)
					logger::error("[HairStrands] Strand pixel shader failed to compile; this hair permutation keeps its cards");
			});
		return nullptr;
	}

	winrt::com_ptr<ID3D11ComputeShader> StrandRenderer::EnsureComputeShader(ComputeShader& a_slot, const wchar_t* a_path, const char* a_failure)
	{
		std::scoped_lock lock(computeShaderMutex);
		if (a_slot.shader || a_slot.requested || a_slot.failed)
			return a_slot.shader;
		a_slot.requested = true;
		globals::shaderCache->EnqueueComputeShaderCompile(a_path, "main", {}, [this, &a_slot, generation = a_slot.generation, a_failure](ID3D11ComputeShader* a_shader) {
			std::scoped_lock lock(computeShaderMutex);
			if (generation != a_slot.generation) {  // ClearShaders ran while this compiled
				if (a_shader)
					a_shader->Release();
				return;
			}
			if (a_shader) {
				a_slot.shader.attach(a_shader);
			} else {
				a_slot.failed = true;
				logger::error("[HairStrands] {}", a_failure);
			}
		});
		return nullptr;
	}

	bool StrandRenderer::EnsureSkinShader()
	{
		return EnsureComputeShader(skinShader, kSkinShaderPath, kSkinShaderFailure) != nullptr;
	}

	bool StrandRenderer::EnsureInstanceBuffers(Instance& a_instance)
	{
		const uint32_t points = a_instance.asset->strandCount * a_instance.asset->pointsPerStrand;
		if (a_instance.skinned && a_instance.skinnedCapacity == points)
			return true;
		a_instance.skinned.reset();
		a_instance.skinnedCapacity = 0;
		a_instance.previousAbsolute.clear();
		try {
			a_instance.skinned = std::make_unique<Buffer>(StructuredDesc(sizeof(SkinnedPoint), points, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0), nullptr, "HairStrands::SkinnedPoints");
			a_instance.skinned->CreateSRV(BufferSRVDesc(points));
			a_instance.skinned->CreateUAV(BufferUAVDesc(points));
			a_instance.skinnedCapacity = points;
			if (!drawCB)
				drawCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<StrandDrawCB>(), "HairStrands::StrandDrawCB");
			if (!skinCB)
				skinCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SkinCB>(), "HairStrands::SkinCB");
		} catch (const std::exception& e) {
			a_instance.skinned.reset();
			a_instance.skinnedCapacity = 0;
			logger::error("[HairStrands] Could not create the strand buffers: {}", e.what());
			return false;
		}
		return true;
	}

	bool StrandRenderer::UpdateLod(Instance& a_instance, RE::BSGeometry* a_geometry)
	{
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const float3 eye = ToFloat3(shadowState.posAdjust.getEye());
		a_instance.distance = (ToFloat3(a_geometry->worldBound.center) - eye).Length();

		// Past the LOD end the cards take over (with a little hysteresis against flicker).
		const float end = settings.lodEnd * (a_instance.lodActive ? kLodHysteresis : 1.0f);
		a_instance.lodActive = a_instance.distance <= end;
		if (!a_instance.lodActive || !a_instance.allowed)
			return false;
		a_instance.simWeight = settings.physics ? 1.0f - Smoothstep(settings.physicsDistance * kSimFadeStart, settings.physicsDistance, a_instance.distance) : 0.0f;

		const auto& asset = *a_instance.asset;
		const auto& style = a_instance.style;
		const float fade = Smoothstep(settings.lodStart, settings.lodEnd, a_instance.distance);

		// Fewer, wider strands with distance: coverage (count x width) stays about the same.
		float fraction = std::clamp(std::lerp(1.0f, settings.minStrandFraction, fade) * settings.densityScale * a_instance.budgetScale, 1e-3f, 1.0f);
		const float widthScale = std::min(1.0f / fraction, settings.maxWidthScale);

		// Strands thinner than the pixel floor are widened by the vertex shader anyway; draw
		// correspondingly fewer so the hair does not thicken with distance. The floor is in
		// rendered pixels: with an upscaler an output pixel is a fraction of one, and strands
		// that thin alias into a faint, see-through fuzz after temporal resolve.
		const float proj11 = std::abs(shadowState.cameraData.getEye().projMat.m[1][1]);
		const auto* graphicsState = globals::game::graphicsState;
		const float renderHeight = Util::ConvertToDynamic(float2(static_cast<float>(graphicsState->screenWidth), static_cast<float>(graphicsState->screenHeight)), true).y;
		const float pixelsPerUnitAtOne = 0.5f * renderHeight * std::max(proj11, 1e-3f);
		a_instance.minWidthPerDistance = settings.minPixelWidth / pixelsPerUnitAtOne;
		const float floorWidth = a_instance.minWidthPerDistance * std::max(a_instance.distance, 1.0f);
		const float rootWidth = style.rootWidth * widthScale;
		if (rootWidth < floorWidth)
			fraction *= rootWidth / floorWidth;

		const uint32_t total = asset.strandCount;
		a_instance.activeStrands = std::clamp(static_cast<uint32_t>(std::lround(total * fraction)), std::min(kMinDrawnStrands, total), total);
		a_instance.widthScale = widthScale;

		// Enough render points per segment for the curls and waves up close, one far away.
		const float spacing = asset.averageLength / std::max(static_cast<float>(asset.pointsPerStrand) - 1.0f, 1.0f);
		float wanted = 2.0f;
		if (style.curlRadius > 0.0f)
			wanted = std::max(wanted, std::ceil(spacing * 8.0f / style.curlLength));
		if (style.waveAmplitude > 0.0f)
			wanted = std::max(wanted, std::ceil(spacing * 6.0f / style.waveLength));
		a_instance.subdivisions = std::clamp(static_cast<uint32_t>(std::lround(std::lerp(wanted, 1.0f, fade))), 1u, std::max(settings.maxSubdivisions, 1u));
		return true;
	}

	bool StrandRenderer::Skin(Instance& a_instance, RE::NiSkinInstance* a_skin)
	{
		auto* skinData = a_skin->skinData.get();
		const uint32_t bones = skinData->GetBoneCount();
		if (bones == 0 || !a_skin->boneWorldTransforms)
			return false;

		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const float3 eye = ToFloat3(shadowState.posAdjust.getEye());
		const float3 previousEye = ToFloat3(shadowState.previousPosAdjust.getEye());

		// Skin-to-world per bone, as the game skins the cards: bone world x skin-to-bone.
		std::vector<float4> absolute(bones * 3);
		for (uint32_t b = 0; b < bones; ++b) {
			const RE::NiTransform* world = a_skin->boneWorldTransforms[b];
			if (!world && a_skin->bones && a_skin->bones[b])
				world = &a_skin->bones[b]->world;
			const RE::NiTransform m = world ? (*world) * skinData->GetBoneDataSkinToBone(b) : skinData->GetBoneDataSkinToBone(b);
			const float translate[3] = { m.translate.x, m.translate.y, m.translate.z };
			for (int r = 0; r < 3; ++r)
				absolute[b * 3 + r] = { m.rotate.entry[r][0] * m.scale, m.rotate.entry[r][1] * m.scale, m.rotate.entry[r][2] * m.scale, translate[r] };
		}

		// Last frame's palette gives the motion vectors; without one (first frame, or a gap)
		// the hair moves only with the camera this frame.
		const bool havePrevious = a_instance.previousAbsolute.size() == absolute.size() && a_instance.previousFrame + 1 == RenderFrame();
		const auto& previous = havePrevious ? a_instance.previousAbsolute : absolute;
		const float eyeRows[3] = { eye.x, eye.y, eye.z };
		const float previousEyeRows[3] = { previousEye.x, previousEye.y, previousEye.z };
		a_instance.paletteData.resize(bones * 6);
		for (uint32_t i = 0; i < bones * 3; ++i) {
			a_instance.paletteData[i] = absolute[i];
			a_instance.paletteData[i].w -= eyeRows[i % 3];
			a_instance.paletteData[bones * 3 + i] = previous[i];
			a_instance.paletteData[bones * 3 + i].w -= previousEyeRows[i % 3];
		}
		const auto skinProgram = EnsureComputeShader(skinShader, kSkinShaderPath, kSkinShaderFailure);
		if (!skinProgram)
			return false;

		auto* context = globals::d3d::context;
		try {
			if (!a_instance.palette || a_instance.paletteBones != bones) {
				a_instance.palette = std::make_unique<Buffer>(StructuredDesc(sizeof(float4), bones * 6, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE), nullptr, "HairStrands::BonePalette");
				a_instance.palette->CreateSRV(BufferSRVDesc(bones * 6));
				a_instance.paletteBones = bones;
			}
		} catch (const std::exception& e) {
			a_instance.palette.reset();
			logger::error("[HairStrands] Could not create the bone palette: {}", e.what());
			return false;
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(a_instance.palette->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		std::memcpy(mapped.pData, a_instance.paletteData.data(), a_instance.paletteData.size() * sizeof(float4));
		context->Unmap(a_instance.palette->resource.get(), 0);

		// The palette is on the GPU: from here on the frame is skinned (and simulated).
		SkinCB cb{};
		cb.pointCount = a_instance.activeStrands * a_instance.asset->pointsPerStrand;
		cb.boneCount = bones;
		cb.pointsPerStrand = a_instance.asset->pointsPerStrand;
		cb.guideCount = a_instance.asset->guideCount;
		const auto simProgram = settings.physics ? EnsureComputeShader(simShader, kSimShaderPath, kSimShaderFailure) : nullptr;
		const bool simulate = simProgram && PrepareSimulation(a_instance, a_skin, absolute, eye, previousEye, cb);
		if (!simulate) {
			a_instance.lastSimFrame = UINT32_MAX;
			// Physics off for this hair: free its guide state. Past the physics distance it is kept.
			if (!settings.physics || !a_instance.style.simulate) {
				a_instance.guideState.reset();
				a_instance.guideCapacity = 0;
			}
		}
		a_instance.previousAbsolute = std::move(absolute);
		a_instance.previousFrame = RenderFrame();

		skinCB->Update(cb);

		// Mid-pass dispatches: put back every compute binding they touch.
		winrt::com_ptr<ID3D11ComputeShader> oldShader;
		context->CSGetShader(oldShader.put(), nullptr, nullptr);
		ID3D11Buffer* oldCB = nullptr;
		context->CSGetConstantBuffers(0, 1, &oldCB);
		ID3D11ShaderResourceView* oldSRVs[6]{};
		context->CSGetShaderResources(0, 6, oldSRVs);
		ID3D11UnorderedAccessView* oldUAV = nullptr;
		context->CSGetUnorderedAccessViews(0, 1, &oldUAV);

		ID3D11Buffer* cbBuffer = skinCB->CB();
		ID3D11ShaderResourceView* headField = (cb.flags & SkinCB::kHeadField) ? a_instance.headField->srv.get() : nullptr;
		ID3D11ShaderResourceView* bodyField = (cb.flags & SkinCB::kBodyField) ? a_instance.bodyField->srv.get() : nullptr;
		ID3D11ShaderResourceView* srvs[6] = { a_instance.asset->restPoints->srv.get(), a_instance.palette->srv.get(), a_instance.asset->strandInfo->srv.get(), nullptr, headField, bodyField };
		context->CSSetConstantBuffers(0, 1, &cbBuffer);
		context->CSSetShaderResources(0, 6, srvs);
		if (simulate) {
			// The guides first: every strand follows one.
			ID3D11UnorderedAccessView* guideUAV = a_instance.guideState->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &guideUAV, nullptr);
			context->CSSetShader(simProgram.get(), nullptr, 0);
			context->Dispatch((cb.guideCount + 63) / 64, 1, 1);
		}
		// The guide state moves from the UAV slot to t3: unbind it as a UAV first.
		ID3D11UnorderedAccessView* skinnedUAV = a_instance.skinned->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &skinnedUAV, nullptr);
		ID3D11ShaderResourceView* guideSRV = simulate ? a_instance.guideState->srv.get() : nullptr;
		context->CSSetShaderResources(3, 1, &guideSRV);
		context->CSSetShader(skinProgram.get(), nullptr, 0);
		context->Dispatch((cb.pointCount + 63) / 64, 1, 1);

		context->CSSetUnorderedAccessViews(0, 1, &oldUAV, nullptr);
		context->CSSetShaderResources(0, 6, oldSRVs);
		context->CSSetConstantBuffers(0, 1, &oldCB);
		context->CSSetShader(oldShader.get(), nullptr, 0);
		for (auto* srv : oldSRVs) {
			if (srv)
				srv->Release();
		}
		if (oldCB)
			oldCB->Release();
		if (oldUAV)
			oldUAV->Release();

		a_instance.skinEye = eye;
		a_instance.skinPreviousEye = previousEye;
		return true;
	}

	bool StrandRenderer::PrepareSimulation(Instance& a_instance, RE::NiSkinInstance* a_skin, const std::vector<float4>& a_palette, const float3& a_eye, const float3& a_previousEye, SkinCB& o_cb)
	{
		const auto& asset = *a_instance.asset;
		const auto& style = a_instance.style;
		// Short hair (area seeding) barely moves: it keeps plain skinning. TressFX pins the first two
		// points of a strand, so it needs a third to move.
		if (!style.simulate || asset.guideCount == 0 || asset.pointsPerStrand < 3 || asset.seedingUsed == SeedMode::Area || a_instance.simWeight <= 0.0f)
			return false;

		const uint32_t guidePoints = asset.guideCount * asset.pointsPerStrand;
		if (!a_instance.guideState || a_instance.guideCapacity != guidePoints) {
			a_instance.guideState.reset();
			a_instance.guideCapacity = 0;
			a_instance.lastSimFrame = UINT32_MAX;
			try {
				a_instance.guideState = std::make_unique<Buffer>(StructuredDesc(sizeof(GuidePoint), guidePoints, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0), nullptr, "HairStrands::GuideState");
				a_instance.guideState->CreateSRV(BufferSRVDesc(guidePoints));
				a_instance.guideState->CreateUAV(BufferUAVDesc(guidePoints));
				a_instance.guideCapacity = guidePoints;
			} catch (const std::exception& e) {
				a_instance.guideState.reset();
				logger::error("[HairStrands] Could not create the guide strand buffer: {}", e.what());
				return false;
			}
		}

		const bool haveHead = asset.headBone >= 0 && static_cast<uint32_t>(asset.headBone) < o_cb.boneCount;
		const uint32_t frameBone = haveHead ? static_cast<uint32_t>(asset.headBone) : 0;
		const bool reset = a_instance.lastSimFrame == UINT32_MAX || a_instance.simAssetSerial != asset.serial || RenderFrame() - a_instance.lastSimFrame > kMaxSimGapFrames;

		o_cb.headBone = frameBone;
		const bool headField = settings.collision && haveHead && a_instance.headField;
		o_cb.flags = SkinCB::kFollow | (reset ? SkinCB::kReset : 0u) | (settings.collision ? SkinCB::kCollide : 0u) | (headField ? SkinCB::kHeadField : 0u);
		o_cb.headFieldCentre = asset.headCentre;
		o_cb.simWeight = a_instance.simWeight;
		// Without a head bone, the full skinning is the only target there is.
		o_cb.guidance = haveHead ? settings.smpGuidance : 1.0f;

		// This frame's steps on the shared clock.
		o_cb.eyeShift = reset ? float3() : a_instance.simEye - a_eye;
		o_cb.previousToCurrent = a_previousEye - a_eye;
		o_cb.steps = frameSteps;
		o_cb.firstStep = firstStepFraction;
		o_cb.stepFraction = stepFraction;
		o_cb.displayAlpha = displayAlpha;
		o_cb.stepTime = kSimStep;
		o_cb.teleportDistance = std::max(kTeleportDistance, kTeleportSpeed * frameDeltaTime);

		// TressFX's simulation settings.
		o_cb.damping = style.damping;
		o_cb.localStiffness = style.localConstraintStiffness;
		o_cb.globalStiffness = style.globalConstraintStiffness;
		o_cb.globalRange = style.globalConstraintsRange;
		o_cb.gravity = style.gravityMagnitude;
		o_cb.vspCoeff = style.vspCoeff;
		o_cb.vspAccelThreshold = style.vspAccelThreshold;
		o_cb.clampPositionDelta = style.clampPositionDelta;
		o_cb.localIterations = style.localConstraintsIterations;
		o_cb.lengthIterations = style.lengthConstraintsIterations;
		o_cb.tipSeparation = style.tipSeparation;
		for (size_t k = 0; k < windCorners.size(); ++k)
			o_cb.wind[k] = windCorners[k] * style.windResponse;
		const BodySkeleton skeleton = FindBodySkeleton(haveHead && a_skin->bones ? a_skin->bones[asset.headBone] : nullptr);
		o_cb.colliderCount = settings.collision ? GatherColliders(a_instance, skeleton, a_palette, frameBone, a_eye, o_cb.colliders) : 0;
		o_cb.bodyColliderCount = settings.collision ? GatherBodyColliders(a_instance, skeleton, a_eye, a_previousEye, o_cb) : 0;
		if (o_cb.bodyColliderCount)
			o_cb.flags |= SkinCB::kBodyField;

		a_instance.simEye = a_eye;
		a_instance.lastSimFrame = RenderFrame();
		a_instance.simAssetSerial = asset.serial;
		++simulatedThisFrame;
		guidesThisFrame += asset.guideCount;
		return true;
	}

	void StrandRenderer::BuildHeadField(Instance& a_instance, RE::BSGeometry* a_geometry, RE::NiSkinInstance* a_skin)
	{
		// The head sphere is fitted inside the hair, so it lies well inside the forehead, face
		// and back of the head: hair pressed that way went into the head before it stopped. The
		// head mesh itself is stored as a radial height field around the hair's skull centre:
		// per direction, the distance to the outermost head surface.
		const auto& asset = *a_instance.asset;
		a_instance.headFieldSerial = asset.serial;
		a_instance.headField.reset();
		const auto log = [&](std::string_view a_text) {
			if (a_instance.isPlayer)
				logger::info("[HairStrands] {}: {}", a_instance.key.ToString(), a_text);
			else
				logger::debug("[HairStrands] {}: {}", a_instance.key.ToString(), a_text);
		};
		auto* userData = a_geometry->GetUserData();
		auto* actor = userData ? userData->As<RE::Actor>() : nullptr;
		if (!actor || asset.headBone < 0 || static_cast<uint32_t>(asset.headBone) >= a_skin->skinData->GetBoneCount())
			return;
		auto* face = FindFaceGeometry(actor);
		auto* faceSkin = face ? face->GetGeometryRuntimeData().skinInstance.get() : nullptr;
		if (!faceSkin || !faceSkin->skinData) {
			log("no skinned head mesh; collision uses the head sphere");
			return;
		}
		HairMeshData mesh;
		std::string error;
		if (!ExtractHairMesh(face, mesh, error)) {
			log(std::format("head mesh {} unreadable ({}); collision uses the head sphere", face->name.c_str(), error));
			return;
		}
		const int32_t faceHead = FindBone(mesh.boneNames, "npc head");
		if (faceHead < 0) {
			log(std::format("head mesh {} is not skinned to the head bone; collision uses the head sphere", face->name.c_str()));
			return;
		}

		// Head mesh skin space -> head bone -> the hair's skin space. Bind poses only: the field
		// rides the head bone, whatever pose either mesh is drawn in.
		const RE::NiTransform toHair = a_skin->skinData->GetBoneDataSkinToBone(asset.headBone).Invert() * faceSkin->skinData->GetBoneDataSkinToBone(faceHead);
		std::vector<float3> points(mesh.positions.size());
		std::vector<uint8_t> onHead(mesh.positions.size(), 0);
		for (size_t v = 0; v < points.size(); ++v) {
			float headWeight = 0.0f;
			for (int i = 0; i < 4; ++i)
				headWeight += mesh.boneIndices[v][i] == faceHead ? mesh.boneWeights[v][i] : 0.0f;
			const auto& position = mesh.positions[v];
			const RE::NiPoint3 p = toHair * RE::NiPoint3(position.x, position.y, position.z);
			points[v] = float3(p.x, p.y, p.z) - asset.headCentre;
			onHead[v] = headWeight >= kHeadFieldMinHeadWeight;
		}

		// Splat points over every head triangle; each texel keeps the outermost.
		constexpr uint32_t size = kHeadFieldSize;
		std::vector<float> field(size * size, 0.0f);
		const auto splat = [&](const float3& a_point) {
			const float radius = a_point.Length();
			if (!(radius > 1e-3f) || radius > kHeadFieldMaxRadius)
				return;
			const float2 uv = OctahedralUV(a_point / radius);
			const uint32_t x = std::min(static_cast<uint32_t>(std::max(uv.x, 0.0f) * size), size - 1);
			const uint32_t y = std::min(static_cast<uint32_t>(std::max(uv.y, 0.0f) * size), size - 1);
			field[y * size + x] = std::max(field[y * size + x], radius);
		};
		for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
			const uint32_t i0 = mesh.indices[t], i1 = mesh.indices[t + 1], i2 = mesh.indices[t + 2];
			if (i0 >= points.size() || i1 >= points.size() || i2 >= points.size() || !onHead[i0] || !onHead[i1] || !onHead[i2])
				continue;
			const float3 a = points[i0], ab = points[i1] - a, ac = points[i2] - a;
			const float longest = std::max({ ab.Length(), ac.Length(), (ac - ab).Length() });
			if (!std::isfinite(longest) || longest > kHeadFieldMaxRadius)
				continue;
			const uint32_t steps = std::clamp(static_cast<uint32_t>(std::ceil(longest / kHeadFieldSampleSpacing)), 1u, 64u);
			for (uint32_t i = 0; i <= steps; ++i) {
				for (uint32_t j = 0; i + j <= steps; ++j)
					splat(a + ab * (static_cast<float>(i) / steps) + ac * (static_cast<float>(j) / steps));
			}
		}

		// Close small holes (eye sockets, the mouth) from their surroundings. A texel needs at
		// least four filled neighbours, so the straight edge of the neck opening does not grow.
		for (uint32_t pass = 0; pass < kHeadFieldFillPasses; ++pass) {
			std::vector<float> filled = field;
			for (uint32_t y = 0; y < size; ++y) {
				for (uint32_t x = 0; x < size; ++x) {
					if (field[y * size + x] > 0.0f)
						continue;
					float sum = 0.0f;
					uint32_t count = 0;
					for (uint32_t ny = y ? y - 1 : 0; ny <= std::min(y + 1, size - 1); ++ny) {
						for (uint32_t nx = x ? x - 1 : 0; nx <= std::min(x + 1, size - 1); ++nx) {
							if (field[ny * size + nx] > 0.0f) {
								sum += field[ny * size + nx];
								++count;
							}
						}
					}
					if (count >= 4)
						filled[y * size + x] = sum / count;
				}
			}
			field.swap(filled);
		}

		std::vector<float> radii;
		std::ranges::copy_if(field, std::back_inserter(radii), [](float a_radius) { return a_radius > 0.0f; });
		const float coverage = static_cast<float>(radii.size()) / field.size();
		if (coverage < kHeadFieldMinCoverage) {
			log(std::format("head mesh {} covers only {:.0f}% of directions; collision uses the head sphere", face->name.c_str(), coverage * 100.0f));
			return;
		}
		// A head round the hair's skull centre has a median radius near the sphere fitted inside
		// the hair. Anything else means the two meshes do not share that centre.
		std::ranges::nth_element(radii, radii.begin() + radii.size() / 2);
		const float median = radii[radii.size() / 2];
		if (!(median > asset.headRadius * kHeadFieldMinMedianRatio && median < asset.headRadius * kHeadFieldMaxMedianRatio)) {
			log(std::format("head mesh {} does not sit round the hair's skull centre (median radius {:.1f}, head sphere {:.1f}); collision uses the head sphere", face->name.c_str(), median, asset.headRadius));
			return;
		}
		// Long ears, muzzles and horns: hair passing into their directions would be thrown out
		// to their tips.
		for (auto& radius : field)
			radius = std::min(radius, median * kHeadFieldMaxRadiusRatio);
		D3D11_SUBRESOURCE_DATA init{ field.data(), 0, 0 };
		try {
			a_instance.headField = std::make_unique<Buffer>(StructuredDesc(sizeof(float), size * size, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0), &init, "HairStrands::HeadField");
			a_instance.headField->CreateSRV(BufferSRVDesc(size * size));
		} catch (const std::exception& e) {
			a_instance.headField.reset();
			logger::error("[HairStrands] Could not create the head field: {}", e.what());
			return;
		}
		log(std::format("head collider from head mesh {} ({:.0f}% of directions)", face->name.c_str(), coverage * 100.0f));
	}

	uint32_t StrandRenderer::GatherColliders(const Instance& a_instance, const BodySkeleton& a_skeleton, const std::vector<float4>& a_palette, uint32_t a_frameBone, const float3& a_eye, float4* o_colliders) const
	{
		uint32_t count = 0;
		const auto add = [&](const float3& a_a, const float3& a_b, float a_radius) {
			if (count >= kMaxColliders || !(a_radius > 0.0f))
				return;
			const float3 a = a_a - a_eye;
			const float3 b = a_b - a_eye;
			o_colliders[count * 2] = { a.x, a.y, a.z, a_radius };
			o_colliders[count * 2 + 1] = { b.x, b.y, b.z, 0.0f };
			++count;
		};

		// The head: without the head field, a sphere round the skull centre, fitted just inside
		// the strands when they were generated.
		const auto& asset = *a_instance.asset;
		if (!a_instance.headField) {
			const float4* rows = &a_palette[a_frameBone * 3];
			const float4 centre(asset.headCentre.x, asset.headCentre.y, asset.headCentre.z, 1.0f);
			const float3 headCentre(rows[0].Dot(centre), rows[1].Dot(centre), rows[2].Dot(centre));
			add(headCentre, headCentre, asset.headRadius * float3(rows[0].x, rows[1].x, rows[2].x).Length());
		}

		// The body, from the head bone down its humanoid skeleton, while no body colliders have been
		// built from the actor's worn meshes. The radii are well inside a body, and the shader never
		// pushes a point further out than its own target lies.
		if (a_instance.bodyField || !a_skeleton.Has(BodySlot::Neck))
			return count;
		const float scale = a_skeleton.end[static_cast<uint32_t>(BodySlot::Neck)]->world.scale;  // the head's
		const auto capsule = [&](BodySlot a_slot, float a_radius) {
			const auto slot = static_cast<uint32_t>(a_slot);
			if (a_skeleton.Has(a_slot))
				add(ToFloat3(a_skeleton.start[slot]->world.translate), ToFloat3(a_skeleton.end[slot]->world.translate), a_radius * scale);
		};
		capsule(BodySlot::Neck, kNeckRadius);
		capsule(BodySlot::Chest, kChestRadius);
		capsule(BodySlot::Back, kBackRadius);
		capsule(BodySlot::LeftShoulder, kShoulderRadius);
		capsule(BodySlot::LeftArm, kArmRadius);
		capsule(BodySlot::RightShoulder, kShoulderRadius);
		capsule(BodySlot::RightArm, kArmRadius);
		return count;
	}

	void StrandRenderer::UpdateBodyField(Instance& a_instance, RE::BSGeometry* a_geometry, RE::NiSkinInstance* a_skin)
	{
		const auto log = [&](std::string_view a_text) {
			if (a_instance.isPlayer)
				logger::info("[HairStrands] {}: {}", a_instance.key.ToString(), a_text);
			else
				logger::debug("[HairStrands] {}: {}", a_instance.key.ToString(), a_text);
		};

		// A finished build replaces the colliders; one at a time per hair.
		if (a_instance.bodyJob.valid()) {
			if (a_instance.bodyJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				return;
			const auto data = a_instance.bodyJob.get();
			a_instance.bodySignature = a_instance.bodyJobSignature;
			a_instance.bodyField.reset();
			a_instance.previousBodyFrame = UINT32_MAX;
			if (!data || !data->error.empty()) {
				log(std::format("no body colliders from its {} worn meshes ({}); collision uses bone capsules", a_instance.bodyJobMeshes, data ? data->error : "no data"));
				return;
			}
			D3D11_SUBRESOURCE_DATA init{ data->field.data(), 0, 0 };
			const uint32_t texels = kBodySlots * kBodyFieldTexels;
			try {
				a_instance.bodyField = std::make_unique<Buffer>(StructuredDesc(sizeof(float3), texels, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0), &init, "HairStrands::BodyField");
				a_instance.bodyField->CreateSRV(BufferSRVDesc(texels));
			} catch (const std::exception& e) {
				a_instance.bodyField.reset();
				logger::error("[HairStrands] Could not create the body colliders: {}", e.what());
				return;
			}
			a_instance.bodyShapes = data->shapes;
			log(std::format("body colliders from {} worn meshes ({} of each map covered)", a_instance.bodyJobMeshes, data->summary));
			return;
		}

		// What the actor wears, now and then; a change builds the colliders again.
		if (a_instance.bodyCheckFrame != UINT32_MAX && RenderFrame() - a_instance.bodyCheckFrame < kBodyCheckFrames)
			return;
		a_instance.bodyCheckFrame = RenderFrame();
		const auto& asset = *a_instance.asset;
		auto* userData = a_geometry->GetUserData();
		auto* actor = userData ? userData->As<RE::Actor>() : nullptr;
		auto* root = actor ? actor->Get3D(false) : nullptr;
		const bool haveHead = asset.headBone >= 0 && static_cast<uint32_t>(asset.headBone) < a_skin->skinData->GetBoneCount() && a_skin->bones;
		const BodySkeleton skeleton = FindBodySkeleton(haveHead ? a_skin->bones[asset.headBone] : nullptr);
		if (!root || !skeleton.Has(BodySlot::Chest)) {
			a_instance.bodyField.reset();
			a_instance.bodySignature = 0;
			return;
		}
		uint64_t signature = 0;
		const auto meshes = FindBodyMeshes(root, actor->GetFaceNodeSkinned(), FindFaceGeometry(actor), signature);
		if (signature == a_instance.bodySignature)
			return;
		BodyFieldInput input;
		std::string error;
		if (!PrepareBodyField(skeleton, meshes, input, error)) {
			a_instance.bodyField.reset();
			a_instance.bodySignature = signature;
			log(std::format("no body colliders: {}; collision uses bone capsules", error));
			return;
		}
		a_instance.bodyJobSignature = signature;
		a_instance.bodyJobMeshes = meshes.size();
		a_instance.bodyJob = std::async(std::launch::async, [input = std::move(input)]() -> std::unique_ptr<BodyFieldData> {
			auto data = std::make_unique<BodyFieldData>();
			try {
				BuildBodyField(input, *data);
			} catch (const std::exception& e) {
				data->error = e.what();  // bad_alloc on an absurd mesh must not reach the render thread
			}
			return data;
		});
	}

	uint32_t StrandRenderer::GatherBodyColliders(Instance& a_instance, const BodySkeleton& a_skeleton, const float3& a_eye, const float3& a_previousEye, SkinCB& o_cb)
	{
		if (!a_instance.bodyField)
			return 0;
		// Last frame's poses give the steps in between (and the followers' previous positions); without
		// them the colliders hold still over the frame.
		const bool havePrevious = a_instance.previousBodyFrame + 1 == RenderFrame();
		const float eye[3] = { a_eye.x, a_eye.y, a_eye.z };
		const float previousEye[3] = { a_previousEye.x, a_previousEye.y, a_previousEye.z };
		uint32_t count = 0;
		for (uint32_t s = 0; s < kBodySlots; ++s) {
			const auto& shape = a_instance.bodyShapes[s];
			const RE::NiAVObject* bone = a_skeleton.start[s];
			if (!shape.present || !bone)
				continue;
			// Field to world: the bone's world transform after the field's axes.
			const RE::NiTransform& world = bone->world;
			const RE::NiMatrix3 axes = world.rotate * shape.boneFromField;
			std::array<float4, 3> current;
			for (int r = 0; r < 3; ++r)
				current[r] = { axes.entry[r][0] * world.scale, axes.entry[r][1] * world.scale, axes.entry[r][2] * world.scale, world.translate[r] };
			const auto& previous = havePrevious ? a_instance.previousBodyFrames[s] : current;
			for (int r = 0; r < 3; ++r) {
				o_cb.bodyFrames[count * 6 + r] = current[r];
				o_cb.bodyFrames[count * 6 + r].w -= eye[r];
				o_cb.bodyFrames[count * 6 + 3 + r] = previous[r];
				o_cb.bodyFrames[count * 6 + 3 + r].w -= previousEye[r];
			}
			o_cb.bodyShapes[count] = { shape.length, shape.bound, std::bit_cast<float>(s), 0.0f };
			a_instance.previousBodyFrames[s] = current;
			++count;
		}
		a_instance.previousBodyFrame = RenderFrame();
		return count;
	}

	ID3D11RasterizerState* StrandRenderer::GetNoCullState(ID3D11RasterizerState* a_current)
	{
		auto& slot = noCullStates[a_current];
		if (slot)
			return slot.get();
		D3D11_RASTERIZER_DESC desc{};
		if (a_current) {
			a_current->GetDesc(&desc);
		} else {
			desc.FillMode = D3D11_FILL_SOLID;
			desc.DepthClipEnable = TRUE;
		}
		// Ribbons face the camera, but their winding depends on the strand's direction.
		desc.CullMode = D3D11_CULL_NONE;
		if (FAILED(globals::d3d::device->CreateRasterizerState(&desc, slot.put())))
			return a_current;
		Util::SetResourceName(slot.get(), "HairStrands::NoCullRasterizer");
		return slot.get();
	}

	ID3D11DepthStencilState* StrandRenderer::GetStrandDepthState(ID3D11DepthStencilState* a_current, bool a_reversedDepth)
	{
		auto& slot = strandDepthStates[a_reversedDepth ? 1 : 0][a_current];
		if (slot)
			return slot.get();
		D3D11_DEPTH_STENCIL_DESC desc{};
		if (a_current) {
			a_current->GetDesc(&desc);
		} else {
			desc.DepthFunc = a_reversedDepth ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
			desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
			desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
			desc.FrontFace = desc.BackFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
		}
		// Alpha-tested hair is shaded with an equal test against its depth prepass, which no
		// strand off the card surface can pass. Strands are opaque: test and write like any.
		desc.DepthEnable = TRUE;
		desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		if (desc.DepthFunc == D3D11_COMPARISON_EQUAL)
			desc.DepthFunc = a_reversedDepth ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
		if (FAILED(globals::d3d::device->CreateDepthStencilState(&desc, slot.put())))
			return a_current;
		Util::SetResourceName(slot.get(), "HairStrands::DepthState");
		return slot.get();
	}

	void StrandRenderer::Draw(Instance& a_instance, ShaderVariant& a_variant, const D3D11_VIEWPORT* a_viewport, bool a_depthOnly)
	{
		auto* context = globals::d3d::context;
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const auto& asset = *a_instance.asset;
		const auto& style = a_instance.style;

		if (a_depthOnly) {
			// Of the passes that hid the cards, only those writing depth need the strands' depth.
			winrt::com_ptr<ID3D11DepthStencilState> depthState;
			UINT ref = 0;
			context->OMGetDepthStencilState(depthState.put(), &ref);
			if (depthState) {
				D3D11_DEPTH_STENCIL_DESC desc{};
				depthState->GetDesc(&desc);
				if (!desc.DepthEnable || desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO)
					return;
			}
			const uint32_t descriptor = globals::state->currentPixelDescriptor;
			if (loggedDepthPasses.insert(descriptor).second)
				logger::info("[HairStrands] Strand depth drawn in Utility pass {:#x}", descriptor);
		}

		StrandDrawCB cb{};
		cb.pointsPerStrand = asset.pointsPerStrand;
		cb.subdivisions = a_instance.subdivisions;
		cb.widthScale = a_instance.widthScale;
		cb.minWidthPerDistance = a_instance.minWidthPerDistance;
		cb.eyeDelta = a_instance.skinEye - ToFloat3(shadowState.posAdjust.getEye());
		cb.previousEyeDelta = a_instance.skinPreviousEye - ToFloat3(shadowState.previousPosAdjust.getEye());
		cb.rootWidth = style.rootWidth;
		cb.tipWidth = style.tipWidth;
		cb.waveAmplitude = style.waveAmplitude;
		cb.waveLength = style.waveLength;
		cb.curlRadius = style.curlRadius;
		cb.curlLength = style.curlLength;
		cb.curlStart = style.curlStart;
		cb.frizz = style.frizz;
		cb.flyaways = style.flyaways;
		cb.curlCoherence = style.clumpStrength;
		drawCB->Update(cb);

		const bool annotate = globals::state->frameAnnotations;
		if (annotate)
			globals::state->BeginPerfEvent(a_depthOnly ? "Hair Strands Depth" : "Hair Strands");

		// Everything below is put back exactly, so the game's cached state stays true. ReverseZ
		// flips the depth test and rasterizer state as it binds them, and its read-back hooks
		// return the unflipped ones: read, set and restore them raw, as bound. An unflipped state
		// restored raw stays bound behind the game's and ReverseZ's backs, and every later draw
		// with the same state tests depth backwards.
		ReverseZ::SetHookPassthrough(true);
		winrt::com_ptr<ID3D11VertexShader> oldVS;
		context->VSGetShader(oldVS.put(), nullptr, nullptr);
		winrt::com_ptr<ID3D11PixelShader> oldPS;
		context->PSGetShader(oldPS.put(), nullptr, nullptr);
		winrt::com_ptr<ID3D11InputLayout> oldLayout;
		context->IAGetInputLayout(oldLayout.put());
		D3D11_PRIMITIVE_TOPOLOGY oldTopology{};
		context->IAGetPrimitiveTopology(&oldTopology);
		ID3D11Buffer* oldCB = nullptr;
		context->VSGetConstantBuffers(7, 1, &oldCB);
		ID3D11ShaderResourceView* oldSRVs[3]{};
		context->VSGetShaderResources(0, 3, oldSRVs);
		// The lighting draw reads its colour from t0: the strands' gap-filled colour texture
		// in place of the card texture.
		ID3D11ShaderResourceView* colour = a_depthOnly ? nullptr : asset.colour.get();
		ID3D11ShaderResourceView* oldColour = nullptr;
		if (colour)
			context->PSGetShaderResources(0, 1, &oldColour);
		winrt::com_ptr<ID3D11RasterizerState> oldRS;
		context->RSGetState(oldRS.put());
		winrt::com_ptr<ID3D11DepthStencilState> oldDepthState;
		UINT stencilRef = 0;
		context->OMGetDepthStencilState(oldDepthState.put(), &stencilRef);
		// Depth values are reversed when the camera's projection is (ReverseZ): its z row is ~0, not ~1.
		const bool reversedDepth = std::abs(shadowState.cameraData.getEye().projMat.m[2][2]) < 0.5f;

		ID3D11Buffer* drawBuffer = drawCB->CB();
		ID3D11ShaderResourceView* srvs[3] = { asset.restPoints->srv.get(), asset.strandInfo->srv.get(), a_instance.skinned->srv.get() };
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		// The depth prepass needs only the strands' depth: the same vertex shader as their lighting
		// draw, so both passes produce the same depth, and no pixel shader.
		context->VSSetShader(a_variant.vs.get(), nullptr, 0);
		context->PSSetShader(a_depthOnly ? nullptr : a_variant.ps.get(), nullptr, 0);
		context->VSSetConstantBuffers(7, 1, &drawBuffer);
		context->VSSetShaderResources(0, 3, srvs);
		if (colour)
			context->PSSetShaderResources(0, 1, &colour);
		context->RSSetState(GetNoCullState(oldRS.get()));
		context->OMSetDepthStencilState(GetStrandDepthState(oldDepthState.get(), reversedDepth), stencilRef);
		if (a_viewport)
			context->RSSetViewports(1, a_viewport);

		const uint32_t renderPoints = (asset.pointsPerStrand - 1) * a_instance.subdivisions + 1;
		context->DrawInstanced(renderPoints * 2, a_instance.activeStrands, 0, 0);

		context->OMSetDepthStencilState(oldDepthState.get(), stencilRef);
		context->RSSetState(oldRS.get());
		context->VSSetShaderResources(0, 3, oldSRVs);
		if (colour) {
			context->PSSetShaderResources(0, 1, &oldColour);
			if (oldColour)
				oldColour->Release();
		}
		context->VSSetConstantBuffers(7, 1, &oldCB);
		context->PSSetShader(oldPS.get(), nullptr, 0);
		context->VSSetShader(oldVS.get(), nullptr, 0);
		context->IASetPrimitiveTopology(oldTopology);
		context->IASetInputLayout(oldLayout.get());
		for (auto* srv : oldSRVs) {
			if (srv)
				srv->Release();
		}
		if (oldCB)
			oldCB->Release();
		ReverseZ::SetHookPassthrough(false);

		if (annotate)
			globals::state->EndPerfEvent();
	}

	void StrandRenderer::OnSetupGeometry(RE::BSRenderPass* a_pass)
	{
		RestoreHiddenViewport();

		// Main world view only: reflections and cubemaps keep the cards.
		auto* state = globals::state;
		if (!state->inWorld || (state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
			return;
		auto* geometry = a_pass->geometry;
		if (!geometry || !geometry->GetUserData())
			return;
		auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
		if (!skin || !skin->skinPartition || !skin->skinData)
			return;
		// Pixel descriptors carry no Skinned bit (the skin instance above is the test). Model-space
		// normal permutations shade from a normal map the strands cannot follow: keep their cards.
		const uint32_t descriptor = state->modifiedPixelDescriptor;
		if (descriptor & static_cast<uint32_t>(LightingFlags::ModelSpaceNormals))
			return;

		Instance* instance = FindOrCreateInstance(a_pass, geometry);
		instance->lastSeenFrame = frame;
		if (instance->layer) {
			const auto [twinGeometry, twin] = FindStrandTwin(*instance);
			if (!twin)
				return;
			HideCards(a_pass);
			// An alpha-tested layer under blended hair (KS Hairdos' "Hl" parts) draws the strands
			// in its own deferred pass: see DrawsTwin.
			if (!DrawsTwin(*instance, *twin) || (a_pass->shaderProperty && a_pass->shaderProperty->alpha < 0.99f))
				return;
			auto* twinSkin = LiveTwinSkin(geometry, twinGeometry, twin->skinInstance, twin->vertexCount);
			auto* variant = GetVariant(descriptor);
			if (!twinSkin || !variant || !EnsureSkinShader() || !PrepareStrands(*twin, twinGeometry, twinSkin))
				return;
			twin->strandDescriptor = descriptor;
			twin->layerDrawFrame = RenderFrame();
			currentInstance = twin;
			currentVariant = variant;
			return;
		}
		if (!instance->isHair)
			return;
		if (instance->styleGeneration == UINT32_MAX) {
			ResolveStyle(*instance);
			if (instance->converted) {
				auto asset = RequestAsset(*instance, a_pass, geometry);
				if (!instance->asset || instance->asset->state != Asset::State::Ready)
					instance->asset = asset;
				else if (asset != instance->asset)
					instance->nextAsset = asset;  // keep drawing the old strands meanwhile
				else
					instance->nextAsset.reset();
			} else {
				instance->asset.reset();
				instance->nextAsset.reset();
			}
		}
		if (!instance->converted)
			return;
		if (instance->nextAsset && instance->nextAsset->state == Asset::State::Ready) {
			instance->asset = std::move(instance->nextAsset);
			instance->nextAsset.reset();
		}
		if (!instance->asset || instance->asset->state != Asset::State::Ready)
			return;
		// A fading actor keeps its cards: strands have no alpha to fade with.
		if (a_pass->shaderProperty && a_pass->shaderProperty->alpha < 0.99f)
			return;
		// Its alpha-tested layer drew the strands this frame: only the cards go.
		if (instance->layerDrawFrame == RenderFrame()) {
			HideCards(a_pass);
			return;
		}

		auto* variant = GetVariant(descriptor);
		if (variant) {
			instance->strandDescriptor = descriptor;
		} else if (instance->lastPrepassFrame == RenderFrame()) {
			// The depth prepass already drew the strands in place of the cards, so the cards
			// cannot come back this frame: while this permutation compiles, shade with the one
			// that drew the prepass.
			variant = FindVariant(instance->strandDescriptor);
		}
		if (!variant || !EnsureSkinShader() || !PrepareStrands(*instance, geometry, skin))
			return;

		currentInstance = instance;
		currentVariant = variant;
		HideCards(a_pass);
	}

	bool StrandRenderer::PrepareStrands(Instance& a_instance, RE::BSGeometry* a_geometry, RE::NiSkinInstance* a_skin)
	{
		// Once per rendered frame, in whichever of the hair's passes comes first.
		if (a_instance.lastSkinnedFrame != RenderFrame()) {
			a_instance.lastSkinnedFrame = RenderFrame();
			a_instance.drawThisFrame = EnsureInstanceBuffers(a_instance) && UpdateLod(a_instance, a_geometry);
			// Only hair drawn and simulated reads its actor's head mesh and worn meshes.
			const auto& asset = *a_instance.asset;
			if (a_instance.drawThisFrame && a_instance.simWeight > 0.0f && settings.collision && a_instance.style.simulate && asset.seedingUsed != SeedMode::Area && asset.guideCount > 0) {
				if (a_instance.headFieldSerial != asset.serial)
					BuildHeadField(a_instance, a_geometry, a_skin);
				UpdateBodyField(a_instance, a_geometry, a_skin);
			}
			a_instance.drawThisFrame = a_instance.drawThisFrame && Skin(a_instance, a_skin);
			if (a_instance.drawThisFrame) {
				strandsThisFrame += a_instance.activeStrands;
				++drawnThisFrame;
			}
		}
		return a_instance.drawThisFrame;
	}

	void StrandRenderer::RestoreHiddenViewport()
	{
		currentPass = nullptr;
		currentInstance = nullptr;
		currentVariant = nullptr;
		currentDepthOnly = false;
		if (cardsHidden) {
			// The last hidden pass never reached RestoreGeometry: never leave the viewport hidden.
			auto& shadowState = globals::game::shadowState->GetRuntimeData();
			shadowState.viewPort = savedViewport;
			shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
			cardsHidden = false;
		}
	}

	void StrandRenderer::OnUtilitySetupGeometry(RE::BSRenderPass* a_pass)
	{
		RestoreHiddenViewport();

		// Shadow maps keep the cards: strands cast no shadows of their own.
		auto* state = globals::state;
		if ((state->currentPixelDescriptor & static_cast<uint32_t>(UtilityFlags::RenderShadowmap)) ||
			(state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
			return;
		auto* geometry = a_pass->geometry;
		if (!geometry)
			return;
		auto it = instances.find(geometry);
		auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
		if (it == instances.end() || !skin || !skin->skinPartition || !skin->skinData || it->second->skinInstance != skin || it->second->vertexCount != skin->skinPartition->vertexCount)
			return;
		Instance& instance = *it->second;

		if (instance.layer) {
			const auto [twinGeometry, twin] = FindStrandTwin(instance);
			if (!twin)
				return;
			HideCards(a_pass);
			// The blended hair over an alpha-tested layer has no depth prepass of its own: the
			// strands' depth goes in the layer's (see DrawsTwin).
			if (!DrawsTwin(instance, *twin) || (a_pass->shaderProperty && a_pass->shaderProperty->alpha < 0.99f))
				return;
			auto* twinSkin = LiveTwinSkin(geometry, twinGeometry, twin->skinInstance, twin->vertexCount);
			auto* variant = FindVariant(twin->strandDescriptor);
			if (!twinSkin || !variant || !EnsureSkinShader() || !PrepareStrands(*twin, twinGeometry, twinSkin))
				return;
			twin->lastPrepassFrame = RenderFrame();
			currentInstance = twin;
			currentVariant = variant;
			currentDepthOnly = true;
			return;
		}

		// The depth prepass takes the strands' depth in place of the cards'. The shadow mask and
		// every other screen-space pass built from this depth then see the strands, not whatever
		// lies behind them. The lighting pass that follows draws the same depth again. The
		// prepass (Main_RenderDepth) runs before the world pass, so inWorld is not set yet; the
		// instance itself, created by a world lighting pass, says this is the world's hair.
		if (!instance.isHair || !instance.converted || !instance.asset || instance.asset->state != Asset::State::Ready)
			return;
		if (a_pass->shaderProperty && a_pass->shaderProperty->alpha < 0.99f)
			return;
		auto* variant = FindVariant(instance.strandDescriptor);
		if (!variant || !EnsureSkinShader() || !PrepareStrands(instance, geometry, skin))
			return;

		instance.lastPrepassFrame = RenderFrame();
		currentInstance = &instance;
		currentVariant = variant;
		currentDepthOnly = true;
		HideCards(a_pass);
	}

	void StrandRenderer::OnEffectSetupGeometry(RE::BSRenderPass* a_pass)
	{
		RestoreHiddenViewport();

		// An effect shader on the hair's shader property draws the cards' geometry again with
		// its own textures: a membrane from a magic effect, or an overlay such as dirt and blood.
		// The strands have no such pass, and the cards must not show: the effect is hidden with
		// them. Reflections keep the cards, and their effects.
		auto* state = globals::state;
		if (state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))
			return;
		auto* geometry = a_pass->geometry;
		if (!geometry)
			return;
		auto it = instances.find(geometry);
		auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
		if (it == instances.end() || !skin || !skin->skinPartition || it->second->skinInstance != skin || it->second->vertexCount != skin->skinPartition->vertexCount)
			return;
		const Instance& instance = *it->second;
		if (instance.layer ? TwinDrawsStrands(instance) : instance.isHair && DrawsStrands(instance))
			HideCards(a_pass);
	}

	void StrandRenderer::HideCards(RE::BSRenderPass* a_pass)
	{
		// The cards still draw (and still cast shadows elsewhere) but into a viewport past the
		// render target, so no fragment of them reaches the pixel shader.
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		savedViewport = shadowState.viewPort;
		shadowState.viewPort = { kHiddenViewportOrigin, kHiddenViewportOrigin, kHiddenViewportSize, kHiddenViewportSize, savedViewport.MinDepth, savedViewport.MaxDepth };
		shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
		currentPass = a_pass;
		cardsHidden = true;
	}

	bool StrandRenderer::GetCardViewport(D3D11_VIEWPORT& o_viewport)
	{
		// The viewport bound now is the hidden one as the game applied it. Any dynamic
		// resolution scale it applied shows in its size; apply the same to the saved one.
		D3D11_VIEWPORT applied{};
		UINT count = 1;
		// Raw, as bound: ReverseZ's read-back hook returns the unflipped depth range.
		ReverseZ::SetHookPassthrough(true);
		globals::d3d::context->RSGetViewports(&count, &applied);
		ReverseZ::SetHookPassthrough(false);
		const float scaleX = applied.Width / kHiddenViewportSize;
		const float scaleY = applied.Height / kHiddenViewportSize;
		if (count == 0 || applied.TopLeftX < kHiddenViewportOrigin * 0.1f || scaleX <= 0.0f || scaleY <= 0.0f) {
			// The hidden viewport never reached the draw: the cards were drawn, and the viewport
			// bound is already the right one for the strands.
			if (!loggedViewportMiss) {
				loggedViewportMiss = true;
				logger::warn("[HairStrands] Hiding the cards did not reach the draw; cards show under the strands");
			}
			return false;
		}
		// Depth range as bound (already mapped by ReverseZ): Draw sets this viewport raw.
		o_viewport = { savedViewport.TopLeftX * scaleX, savedViewport.TopLeftY * scaleY, savedViewport.Width * scaleX, savedViewport.Height * scaleY, applied.MinDepth, applied.MaxDepth };
		return true;
	}

	void StrandRenderer::OnRestoreGeometry(RE::BSRenderPass* a_pass)
	{
		if (a_pass != currentPass) {
			currentPass = nullptr;
			currentInstance = nullptr;
			currentVariant = nullptr;
			currentDepthOnly = false;
			return;
		}
		if (currentInstance && currentVariant) {
			D3D11_VIEWPORT viewport{};
			const bool haveViewport = cardsHidden && GetCardViewport(viewport);
			Draw(*currentInstance, *currentVariant, haveViewport ? &viewport : nullptr, currentDepthOnly);
		}
		if (cardsHidden) {
			auto& shadowState = globals::game::shadowState->GetRuntimeData();
			shadowState.viewPort = savedViewport;
			shadowState.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
			cardsHidden = false;
		}
		currentPass = nullptr;
		currentInstance = nullptr;
		currentVariant = nullptr;
		currentDepthOnly = false;
	}
}
