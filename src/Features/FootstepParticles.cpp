#include "FootstepParticles.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/Effects11.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Features/InverseSquareLighting.h"
#include "Features/LightLimitFix.h"
#include "Features/Skylighting.h"
#include "Features/SnowCover.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainShadows.h"
#include "Features/WetnessEffects.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.footstep_particles."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FootstepParticles::Settings,
	Enabled,
	IncludeNPCs,
	UseWetnessEffects,
	UseSnowCover,
	MaxDistance,
	DustIntensity,
	SnowIntensity,
	SplashIntensity,
	ParticleSize,
	Lifetime,
	Opacity,
	SurfaceOverride)

namespace
{
	constexpr uint32_t kSlotsPerEvent = 64;
	constexpr uint32_t kMaxParticles = 16384;
	constexpr uint32_t kMaxEventsPerFrame = 64;
	constexpr uint32_t kMaxPendingFootsteps = 256;
	constexpr float kGravity = 686.0f;
	constexpr float kMaxDeltaTime = 0.1f;
	constexpr float kMaxParticleLifetime = 2.2f;
	constexpr float kTeleportDistance = 4096.0f;
	constexpr float kSoftDistance = 12.0f;

	constexpr uint32_t kTagLeft = 1u << 0;
	constexpr uint32_t kTagRight = 1u << 1;
	constexpr uint32_t kTagFront = 1u << 2;
	constexpr uint32_t kTagBack = 1u << 3;
	constexpr uint32_t kTagSprint = 1u << 4;
	constexpr uint32_t kTagJump = 1u << 5;
	constexpr uint32_t kTagLand = 1u << 6;
	constexpr uint32_t kTagScuff = 1u << 7;

	constexpr uint32_t kFrameFlagWetness = 1u << 0;
	constexpr uint32_t kFrameFlagSnowCover = 1u << 1;
	constexpr uint32_t kFrameFlagFog = 1u << 2;
	constexpr uint32_t kFrameFlagInterior = 1u << 3;
	constexpr uint32_t kEventFlagLanding = 1u << 0;

	constexpr uint32_t kConstantSlots = 13;
	constexpr uint32_t kCSResourceSlots = 6;
	constexpr uint32_t kPSResourceSlots = 2;

	constexpr std::array<const char*, static_cast<size_t>(FootstepParticles::Surface::Count) + 1> kSurfaceNames{
		"Auto", "Hard", "Dirt", "Gravel", "Sand", "Grass", "Mud", "Snow", "Ice", "Ash", "Water"
	};

	class FootstepEventSink final : public RE::BSTEventSink<RE::BGSFootstepEvent>
	{
	public:
		static FootstepEventSink* GetSingleton()
		{
			static FootstepEventSink sink;
			return &sink;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::BGSFootstepEvent* a_event, RE::BSTEventSource<RE::BGSFootstepEvent>*) override
		{
			if (a_event)
				globals::features::footstepParticles.QueueFootstep(a_event->actor, a_event->tag);
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	struct BSShaderAccumulator_RenderEffects
	{
		static void thunk(RE::BSShaderAccumulator* a_accumulator, uint32_t a_renderFlags)
		{
			globals::features::footstepParticles.Render(a_accumulator);
			func(a_accumulator, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PipelineBackup
	{
		ID3D11ComputeShader* cs = nullptr;
		ID3D11Buffer* csConstants[kConstantSlots] = {};
		ID3D11ShaderResourceView* csResources[kCSResourceSlots] = {};
		ID3D11UnorderedAccessView* csUAV = nullptr;
		ID3D11SamplerState* csSampler = nullptr;

		ID3D11VertexShader* vs = nullptr;
		ID3D11Buffer* vsConstants[kConstantSlots] = {};
		ID3D11ShaderResourceView* vsResource = nullptr;

		ID3D11PixelShader* ps = nullptr;
		ID3D11Buffer* psConstants[kConstantSlots] = {};
		ID3D11ShaderResourceView* psResources[kPSResourceSlots] = {};
		ID3D11SamplerState* psSampler = nullptr;

		ID3D11InputLayout* inputLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

		ID3D11RasterizerState* rasterizer = nullptr;
		UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

		ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* depthStencil = nullptr;
		ID3D11BlendState* blend = nullptr;
		FLOAT blendFactor[4] = {};
		UINT sampleMask = 0;
		ID3D11DepthStencilState* depthState = nullptr;
		UINT stencilRef = 0;

		void Save(ID3D11DeviceContext* a_context)
		{
			a_context->CSGetShader(&cs, nullptr, nullptr);
			a_context->CSGetConstantBuffers(0, kConstantSlots, csConstants);
			a_context->CSGetShaderResources(0, kCSResourceSlots, csResources);
			a_context->CSGetUnorderedAccessViews(0, 1, &csUAV);
			a_context->CSGetSamplers(0, 1, &csSampler);

			a_context->VSGetShader(&vs, nullptr, nullptr);
			a_context->VSGetConstantBuffers(0, kConstantSlots, vsConstants);
			a_context->VSGetShaderResources(0, 1, &vsResource);

			a_context->PSGetShader(&ps, nullptr, nullptr);
			a_context->PSGetConstantBuffers(0, kConstantSlots, psConstants);
			a_context->PSGetShaderResources(0, kPSResourceSlots, psResources);
			a_context->PSGetSamplers(0, 1, &psSampler);

			a_context->IAGetInputLayout(&inputLayout);
			a_context->IAGetPrimitiveTopology(&topology);

			a_context->RSGetState(&rasterizer);
			viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			a_context->RSGetViewports(&viewportCount, viewports);

			a_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, &depthStencil);
			a_context->OMGetBlendState(&blend, blendFactor, &sampleMask);
			a_context->OMGetDepthStencilState(&depthState, &stencilRef);
		}

		void Restore(ID3D11DeviceContext* a_context)
		{
			a_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, depthStencil);
			a_context->OMSetBlendState(blend, blendFactor, sampleMask);
			a_context->OMSetDepthStencilState(depthState, stencilRef);

			a_context->CSSetShader(cs, nullptr, 0);
			a_context->CSSetConstantBuffers(0, kConstantSlots, csConstants);
			a_context->CSSetShaderResources(0, kCSResourceSlots, csResources);
			a_context->CSSetUnorderedAccessViews(0, 1, &csUAV, nullptr);
			a_context->CSSetSamplers(0, 1, &csSampler);

			a_context->VSSetShader(vs, nullptr, 0);
			a_context->VSSetConstantBuffers(0, kConstantSlots, vsConstants);
			a_context->VSSetShaderResources(0, 1, &vsResource);

			a_context->PSSetShader(ps, nullptr, 0);
			a_context->PSSetConstantBuffers(0, kConstantSlots, psConstants);
			a_context->PSSetShaderResources(0, kPSResourceSlots, psResources);
			a_context->PSSetSamplers(0, 1, &psSampler);

			a_context->IASetInputLayout(inputLayout);
			a_context->IASetPrimitiveTopology(topology);

			a_context->RSSetState(rasterizer);
			a_context->RSSetViewports(viewportCount, viewports);

			Release();
		}

		void Release()
		{
			auto release = [](auto*& a_pointer) {
				if (a_pointer) {
					a_pointer->Release();
					a_pointer = nullptr;
				}
			};
			release(cs);
			for (auto& buffer : csConstants)
				release(buffer);
			for (auto& view : csResources)
				release(view);
			release(csUAV);
			release(csSampler);
			release(vs);
			for (auto& buffer : vsConstants)
				release(buffer);
			release(vsResource);
			release(ps);
			for (auto& buffer : psConstants)
				release(buffer);
			for (auto& view : psResources)
				release(view);
			release(psSampler);
			release(inputLayout);
			release(rasterizer);
			for (auto& view : renderTargets)
				release(view);
			release(depthStencil);
			release(blend);
			release(depthState);
		}
	};

	bool ContainsNoCase(std::string_view a_haystack, std::string_view a_needle)
	{
		if (a_needle.empty() || a_haystack.size() < a_needle.size())
			return false;
		auto it = std::search(a_haystack.begin(), a_haystack.end(), a_needle.begin(), a_needle.end(), [](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
		});
		return it != a_haystack.end();
	}

	uint32_t ClassifyTag(std::string_view a_tag)
	{
		uint32_t flags = 0;
		if (ContainsNoCase(a_tag, "left"))
			flags |= kTagLeft;
		if (ContainsNoCase(a_tag, "right"))
			flags |= kTagRight;
		if (ContainsNoCase(a_tag, "front"))
			flags |= kTagFront;
		if (ContainsNoCase(a_tag, "back"))
			flags |= kTagBack;
		if (ContainsNoCase(a_tag, "sprint"))
			flags |= kTagSprint;
		if (ContainsNoCase(a_tag, "jumpup"))
			flags |= kTagJump;
		if (ContainsNoCase(a_tag, "jumpdown") || ContainsNoCase(a_tag, "land"))
			flags |= kTagLand;
		if (ContainsNoCase(a_tag, "scuff"))
			flags |= kTagScuff;
		return flags;
	}

	FootstepParticles::Surface ClassifyMaterial(RE::MATERIAL_ID a_material)
	{
		using Surface = FootstepParticles::Surface;
		switch (a_material) {
		case RE::MATERIAL_ID::kDirt:
			return Surface::Dirt;
		case RE::MATERIAL_ID::kGravel:
			return Surface::Gravel;
		case RE::MATERIAL_ID::kSand:
			return Surface::Sand;
		case RE::MATERIAL_ID::kGrass:
			return Surface::Grass;
		case RE::MATERIAL_ID::kMud:
			return Surface::Mud;
		case RE::MATERIAL_ID::kSnow:
		case RE::MATERIAL_ID::kSnowStairs:
			return Surface::Snow;
		case RE::MATERIAL_ID::kIce:
		case RE::MATERIAL_ID::kIceForm:
			return Surface::Ice;
		case RE::MATERIAL_ID::kAsh:
			return Surface::Ash;
		case RE::MATERIAL_ID::kWater:
		case RE::MATERIAL_ID::kWaterPuddle:
			return Surface::Water;
		default:
			return Surface::Hard;
		}
	}

	float4 ToFloat4(const RE::NiColor& a_color)
	{
		return { a_color.red, a_color.green, a_color.blue, 1.0f };
	}
}

void FootstepParticles::SetupResources()
{
	auto device = globals::d3d::device;

	particleBuffer = eastl::make_unique<Buffer>(StructuredBufferDesc<Particle>(kMaxParticles, true, false), nullptr, "FootstepParticles::Particles");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = kMaxParticles;
		particleBuffer->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.NumElements = kMaxParticles;
		particleBuffer->CreateUAV(uavDesc);
	}

	eventBuffer = eastl::make_unique<Buffer>(StructuredBufferDesc<FootstepEvent>(kMaxEventsPerFrame, false, true), nullptr, "FootstepParticles::Events");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = kMaxEventsPerFrame;
		eventBuffer->CreateSRV(srvDesc);
	}

	frameConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<FrameData>(), "FootstepParticles::FrameData");

	{
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
		Util::SetResourceName(linearSampler.get(), "FootstepParticles::LinearSampler");
	}

	{
		D3D11_BLEND_DESC blendDesc{};
		auto& target = blendDesc.RenderTarget[0];
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_ONE;
		target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		target.BlendOp = D3D11_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D11_BLEND_ZERO;
		target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, blendState.put()));
		Util::SetResourceName(blendState.get(), "FootstepParticles::BlendState");
	}

	{
		D3D11_RASTERIZER_DESC rasterizerDesc{};
		rasterizerDesc.FillMode = D3D11_FILL_SOLID;
		rasterizerDesc.CullMode = D3D11_CULL_NONE;
		rasterizerDesc.DepthClipEnable = TRUE;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put()));
		Util::SetResourceName(rasterizerState.get(), "FootstepParticles::RasterizerState");
	}

	{
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = FALSE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthStencilState.put()));
		Util::SetResourceName(depthStencilState.get(), "FootstepParticles::DepthStencilState");
	}

	clearRequested = true;
}

void FootstepParticles::ClearShaderCache()
{
	spawnCS = nullptr;
	simulateCS = nullptr;
	particleVS = nullptr;
	particlePS = nullptr;
	shaderCompileFailed = false;
}

std::vector<std::pair<const char*, const char*>> FootstepParticles::GetFeatureDefines() const
{
	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::features::skylighting.loaded)
		defines.push_back({ "SKYLIGHTING", nullptr });
	if (globals::features::terrainBlending.loaded)
		defines.push_back({ "TERRAIN_BLENDING", nullptr });
	if (globals::features::effects11.loaded)
		defines.push_back({ "EFFECTS11", nullptr });
	return defines;
}

ID3D11ComputeShader* FootstepParticles::GetSpawnCS()
{
	if (!spawnCS && !shaderCompileFailed) {
		spawnCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FootstepParticles\\FootstepSpawnCS.hlsl", GetFeatureDefines(), "cs_5_0")));
		shaderCompileFailed = !spawnCS;
	}
	return spawnCS.get();
}

ID3D11ComputeShader* FootstepParticles::GetSimulateCS()
{
	if (!simulateCS && !shaderCompileFailed) {
		simulateCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FootstepParticles\\FootstepSimulateCS.hlsl", {}, "cs_5_0")));
		shaderCompileFailed = !simulateCS;
	}
	return simulateCS.get();
}

ID3D11VertexShader* FootstepParticles::GetParticleVS()
{
	if (!particleVS && !shaderCompileFailed) {
		particleVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\FootstepParticles\\FootstepParticleVS.hlsl", {}, "vs_5_0")));
		shaderCompileFailed = !particleVS;
	}
	return particleVS.get();
}

ID3D11PixelShader* FootstepParticles::GetParticlePS()
{
	if (!particlePS && !shaderCompileFailed) {
		auto defines = GetFeatureDefines();
		if (globals::features::ibl.loaded)
			defines.push_back({ "IBL", nullptr });
		if (globals::features::exponentialHeightFog.loaded)
			defines.push_back({ "EXP_HEIGHT_FOG", nullptr });
		if (globals::features::lightLimitFix.loaded)
			defines.push_back({ "LIGHT_LIMIT_FIX", nullptr });
		if (globals::features::lightLimitFix.loaded && globals::features::inverseSquareLighting.loaded)
			defines.push_back({ "ISL", nullptr });
		if (globals::features::terrainShadows.loaded)
			defines.push_back({ "TERRAIN_SHADOWS", nullptr });
		if (globals::features::cloudShadows.loaded)
			defines.push_back({ "CLOUD_SHADOWS", nullptr });
		particlePS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\FootstepParticles\\FootstepParticlePS.hlsl", defines, "ps_5_0")));
		shaderCompileFailed = !particlePS;
	}
	return particlePS.get();
}

void FootstepParticles::PostPostLoad()
{
	stl::detour_thunk<BSShaderAccumulator_RenderEffects>(REL::RelocationID(99940, 106585));
	logger::info("[FootstepParticles] Installed hooks");
}

void FootstepParticles::DataLoaded()
{
	if (auto manager = RE::BGSFootstepManager::GetSingleton()) {
		manager->AddEventSink(FootstepEventSink::GetSingleton());
		logger::info("[FootstepParticles] Registered footstep event sink");
	} else {
		logger::warn("[FootstepParticles] BGSFootstepManager unavailable; footsteps will not spawn particles");
	}
}

void FootstepParticles::QueueFootstep(RE::ActorHandle a_actor, const RE::BSFixedString& a_tag)
{
	if (!loaded || !settings.Enabled)
		return;

	const uint32_t tagFlags = ClassifyTag(std::string_view(a_tag.c_str()));
	std::scoped_lock lock(pendingMutex);
	if (pendingFootsteps.size() < kMaxPendingFootsteps)
		pendingFootsteps.push_back({ a_actor, tagFlags });
}

bool FootstepParticles::BuildEvent(const PendingFootstep& a_pending, const RE::NiPoint3& a_cameraPosition, FootstepEvent& a_event)
{
	auto actorPointer = a_pending.actor.get();
	auto actor = actorPointer.get();
	if (!actor || !actor->Is3DLoaded() || actor->IsDisabled())
		return false;

	const bool isPlayer = actor->IsPlayerRef();
	if (!isPlayer && !settings.IncludeNPCs)
		return false;

	const RE::NiPoint3 actorPosition = actor->GetPosition();
	const float maxDistance = std::max(settings.MaxDistance, 256.0f);
	if (a_cameraPosition.GetSquaredDistance(actorPosition) > maxDistance * maxDistance)
		return false;

	const uint32_t tag = a_pending.tagFlags;
	const float heading = actor->GetAngleZ();
	const RE::NiPoint3 forward{ std::sin(heading), std::cos(heading), 0.0f };
	const RE::NiPoint3 rightDirection{ std::cos(heading), -std::sin(heading), 0.0f };
	const float height = actor->GetHeight();
	const float scale = std::clamp(std::isfinite(height) && height > 0.0f ? height / 128.0f : 1.0f, 0.35f, 4.0f);

	RE::NiPoint3 footPosition = actorPosition;
	bool foundNode = false;
	auto camera = RE::PlayerCamera::GetSingleton();
	const bool firstPerson = isPlayer && camera && camera->IsInFirstPerson();
	if (!firstPerson && (tag & (kTagLeft | kTagRight))) {
		if (auto root = actor->Get3D(false)) {
			static const RE::BSFixedString leftFoot{ "NPC L Foot [Lft ]" };
			static const RE::BSFixedString rightFoot{ "NPC R Foot [Rft ]" };
			if (auto node = root->GetObjectByName((tag & kTagLeft) ? leftFoot : rightFoot)) {
				footPosition = node->world.translate;
				foundNode = true;
			}
		}
	}
	if (!foundNode) {
		const float side = (tag & kTagLeft) ? -1.0f : ((tag & kTagRight) ? 1.0f : 0.0f);
		const float along = (tag & kTagFront) ? 0.3f : ((tag & kTagBack) ? -0.3f : 0.0f);
		footPosition += rightDirection * (side * 9.0f * scale) + forward * (along * 40.0f * scale);
	}
	footPosition.z = actorPosition.z;

	RE::MATERIAL_ID material = RE::MATERIAL_ID::kNone;
	if (auto controller = actor->GetCharController())
		material = controller->surfaceMaterial;

	if (auto tes = globals::game::tes) {
		float landHeight = 0.0f;
		if (tes->GetLandHeight(footPosition, landHeight)) {
			const float landOffset = std::abs(landHeight - actorPosition.z);
			if (landOffset < 12.0f * scale)
				footPosition.z = landHeight;
			if (landOffset < 12.0f * scale && (material == RE::MATERIAL_ID::kNone || landOffset < 4.0f * scale)) {
				const auto landMaterial = tes->GetLandMaterialType(footPosition);
				if (landMaterial != RE::MATERIAL_ID::kNone)
					material = landMaterial;
			}
		}
	}

	if (auto cell = actor->GetParentCell()) {
		float waterHeight = 0.0f;
		if (cell->GetWaterHeight(footPosition, waterHeight) && waterHeight > footPosition.z + 2.0f)
			return false;
	}

	const Surface surface = settings.SurfaceOverride > 0 && settings.SurfaceOverride <= static_cast<uint>(Surface::Count) ?
	                            static_cast<Surface>(settings.SurfaceOverride - 1) :
	                            ClassifyMaterial(material);

	RE::NiPoint3 velocity;
	actor->GetLinearVelocity(velocity);
	if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z))
		velocity = {};
	const float horizontalSpeed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);

	float strength = std::clamp(0.35f + horizontalSpeed / 420.0f, 0.35f, 1.6f);
	if (tag & kTagSprint)
		strength *= 1.2f;
	if (tag & kTagScuff)
		strength *= 0.5f;
	if (tag & kTagJump)
		strength *= 0.8f;
	if (actor->IsSneaking())
		strength *= 0.6f;

	uint flags = 0;
	if (tag & kTagLand) {
		flags |= kEventFlagLanding;
		strength = std::max(strength, 1.0f);
	}

	if (isPlayer) {
		debug.lastPlayerMaterial = std::string(RE::MaterialIDToString(material));
		debug.lastPlayerSurface = surface;
	}

	eventSeed = eventSeed * 1664525u + 1013904223u;

	a_event.Position = { footPosition.x, footPosition.y, footPosition.z };
	a_event.FirstSlot = nextSlot;
	a_event.Velocity = { velocity.x, velocity.y, velocity.z };
	a_event.Surface = static_cast<uint>(surface);
	a_event.Forward = { forward.x, forward.y };
	a_event.Strength = strength;
	a_event.Scale = scale;
	a_event.Seed = eventSeed;
	a_event.Flags = flags;
	a_event.Pad = { 0.0f, 0.0f };

	nextSlot = (nextSlot + kSlotsPerEvent) % kMaxParticles;
	return true;
}

void FootstepParticles::BuildEvents(const RE::NiPoint3& a_cameraPosition)
{
	frameEvents.clear();

	std::vector<PendingFootstep> pending;
	{
		std::scoped_lock lock(pendingMutex);
		pending.swap(pendingFootsteps);
	}

	if (testBurstQueued) {
		testBurstQueued = false;
		if (auto player = globals::game::player) {
			const auto handle = player->GetHandle();
			pending.push_back({ handle, kTagLeft });
			pending.push_back({ handle, kTagRight | kTagLand });
		}
	}

	for (const auto& footstep : pending) {
		if (frameEvents.size() >= kMaxEventsPerFrame)
			break;
		FootstepEvent footstepEvent{};
		if (BuildEvent(footstep, a_cameraPosition, footstepEvent))
			frameEvents.push_back(footstepEvent);
	}

	debug.eventsThisSecond += static_cast<uint32_t>(frameEvents.size());
}

FootstepParticles::FrameData FootstepParticles::BuildFrameData(float a_deltaTime, uint32_t a_eventCount) const
{
	FrameData data{};

	auto& wetnessEffects = globals::features::wetnessEffects;
	if (settings.UseWetnessEffects && wetnessEffects.loaded && wetnessEffects.settings.EnableWetnessEffects)
		data.FrameFlags |= kFrameFlagWetness;

	auto& snowCover = globals::features::snowCover;
	if (settings.UseSnowCover && snowCover.loaded && snowCover.wsettings.EnableSnowCover && snowCover.views[6])
		data.FrameFlags |= kFrameFlagSnowCover;

	if (Util::IsInterior())
		data.FrameFlags |= kFrameFlagInterior;

	if (auto smState = globals::game::smState) {
		if (auto sceneNode = smState->shadowSceneNode[0]) {
			if (auto& fog = sceneNode->GetRuntimeData().fogProperty) {
				const float range = fog->farDistance - fog->nearDistance;
				if (range > 1.0f && std::isfinite(range)) {
					data.FogParams = { fog->nearDistance / range, 1.0f / range, std::max(fog->power, 0.0f), std::clamp(fog->clamp, 0.0f, 1.0f) };
					data.FogNearColor = ToFloat4(fog->nearColor);
					data.FogFarColor = ToFloat4(fog->farColor);
					auto sky = globals::game::sky;
					const bool colorsMissing = data.FogNearColor.x + data.FogNearColor.y + data.FogNearColor.z + data.FogFarColor.x + data.FogFarColor.y + data.FogFarColor.z <= 0.0f;
					if (colorsMissing && sky) {
						data.FogNearColor = ToFloat4(sky->skyColor[RE::TESWeather::ColorTypes::kFogNear]);
						data.FogFarColor = ToFloat4(sky->skyColor[RE::TESWeather::ColorTypes::kFogFar]);
					}
					data.FrameFlags |= kFrameFlagFog;
				}
			}
		}
	}

	data.DeltaTime = a_deltaTime;
	data.Gravity = kGravity;
	data.EventCount = a_eventCount;
	data.MaxParticles = kMaxParticles;
	data.DustIntensity = settings.DustIntensity;
	data.SnowIntensity = settings.SnowIntensity;
	data.SplashIntensity = settings.SplashIntensity;
	data.SizeScale = settings.ParticleSize;
	data.LifetimeScale = settings.Lifetime;
	data.Opacity = settings.Opacity;
	data.SoftDistance = kSoftDistance;
	data.Time = simulationTime;

	if (auto sky = globals::game::sky; sky && !(data.FrameFlags & kFrameFlagInterior)) {
		const float windSpeed = std::clamp(sky->windSpeed, 0.0f, 1.0f) * 60.0f;
		data.Wind = { std::sin(sky->windAngle) * windSpeed, std::cos(sky->windAngle) * windSpeed };
	}

	return data;
}

void FootstepParticles::Render(RE::BSShaderAccumulator* a_accumulator)
{
	if (!loaded || !a_accumulator)
		return;

	if (!settings.Enabled) {
		std::scoped_lock lock(pendingMutex);
		pendingFootsteps.clear();
		return;
	}

	auto state = globals::state;
	if (!state || !state->inWorld || !globals::shaderCache->IsEnabled())
		return;
	if (state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))
		return;

	auto smState = globals::game::smState;
	const auto* accumulatorData = a_accumulator->GetRuntimeData();
	if (!accumulatorData || accumulatorData->firstPerson || !smState || accumulatorData->activeShadowSceneNode != smState->shadowSceneNode[0])
		return;

	if (lastRenderedFrame == state->frameCount)
		return;
	lastRenderedFrame = state->frameCount;

	if (!particleBuffer || !eventBuffer || !frameConstants)
		return;

	const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
	const float deltaTime = paused ? 0.0f : std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, kMaxDeltaTime);
	simulationTime += deltaTime;

	debug.secondTimer += deltaTime;
	if (debug.secondTimer >= 1.0f) {
		debug.eventsLastSecond = debug.eventsThisSecond;
		debug.eventsThisSecond = 0;
		debug.secondTimer = 0.0f;
	}

	const RE::NiPoint3 cameraPosition = Util::GetEyePosition();
	bool clear = clearRequested;
	if (hasLastCamera && cameraPosition.GetDistance(lastCameraPosition) > kTeleportDistance)
		clear = true;
	lastCameraPosition = cameraPosition;
	hasLastCamera = true;

	BuildEvents(cameraPosition);
	if (!frameEvents.empty())
		lastSpawnTime = simulationTime;

	const bool active = !frameEvents.empty() || (simulationTime - lastSpawnTime) < kMaxParticleLifetime * std::max(settings.Lifetime, 0.1f) + 0.5f;
	if (!active && !clear)
		return;

	Dispatch(deltaTime, clear);
}

void FootstepParticles::Dispatch(float a_deltaTime, bool a_clear)
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	if (!context || !renderer)
		return;

	auto spawn = GetSpawnCS();
	auto simulate = GetSimulateCS();
	auto vertexShader = GetParticleVS();
	auto pixelShader = GetParticlePS();
	if (!spawn || !simulate || !vertexShader || !pixelShader)
		return;

	auto& runtimeData = renderer->GetRuntimeData();
	auto mainTarget = runtimeData.renderTargets[globals::deferred->forwardRenderTargets[0]];
	auto albedo = runtimeData.renderTargets[ALBEDO];
	auto normalRoughness = runtimeData.renderTargets[NORMALROUGHNESS];
	auto shadowMask = runtimeData.renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
	auto sceneDepth = Util::GetCurrentSceneDepthSRV(false);
	if (!mainTarget.RTV || !sceneDepth)
		return;

	auto& snowCover = globals::features::snowCover;
	auto& skylighting = globals::features::skylighting;
	const uint32_t eventCount = static_cast<uint32_t>(frameEvents.size());

	PipelineBackup backup;
	backup.Save(context);

	auto profiler = globals::profiler;
	profiler->BeginPass("FootstepParticles");

	if (a_clear) {
		const UINT zeros[4] = {};
		context->ClearUnorderedAccessViewUint(particleBuffer->uav.get(), zeros);
		clearRequested = false;
	}

	frameConstants->Update(BuildFrameData(a_deltaTime, eventCount));

	if (eventCount > 0) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		DX::ThrowIfFailed(context->Map(eventBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		memcpy_s(mapped.pData, sizeof(FootstepEvent) * kMaxEventsPerFrame, frameEvents.data(), sizeof(FootstepEvent) * eventCount);
		context->Unmap(eventBuffer->resource.get(), 0);
	}

	context->OMSetRenderTargets(0, nullptr, nullptr);

	ID3D11Buffer* frameBuffer = frameConstants->CB();
	ID3D11Buffer* perFrame = *globals::game::perFrame.get();
	ID3D11Buffer* sharedBuffers[2] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	ID3D11SamplerState* sampler = linearSampler.get();
	ID3D11UnorderedAccessView* particleUAV = particleBuffer->uav.get();

	context->CSSetConstantBuffers(0, 1, &frameBuffer);
	context->CSSetConstantBuffers(5, 2, sharedBuffers);
	context->CSSetConstantBuffers(12, 1, &perFrame);
	context->CSSetSamplers(0, 1, &sampler);
	context->CSSetUnorderedAccessViews(0, 1, &particleUAV, nullptr);

	if (eventCount > 0) {
		ID3D11ShaderResourceView* resources[kCSResourceSlots] = {
			sceneDepth,
			albedo.SRV,
			normalRoughness.SRV,
			snowCover.loaded ? snowCover.views[6].get() : nullptr,
			eventBuffer->srv.get(),
			skylighting.loaded && skylighting.texProbeArray ? skylighting.texProbeArray->srv.get() : nullptr
		};
		context->CSSetShaderResources(0, kCSResourceSlots, resources);
		context->CSSetShader(spawn, nullptr, 0);
		context->Dispatch(eventCount, 1, 1);

		ID3D11ShaderResourceView* nullResources[kCSResourceSlots] = {};
		context->CSSetShaderResources(0, kCSResourceSlots, nullResources);
	}

	context->CSSetShader(simulate, nullptr, 0);
	context->Dispatch(kMaxParticles / 64, 1, 1);

	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);

	D3D11_TEXTURE2D_DESC textureDesc{};
	mainTarget.texture->GetDesc(&textureDesc);
	const float2 resolution = Util::ConvertToDynamic(float2{ static_cast<float>(textureDesc.Width), static_cast<float>(textureDesc.Height) });
	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, resolution.x, resolution.y, 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);

	ID3D11RenderTargetView* renderTarget = mainTarget.RTV;
	context->OMSetRenderTargets(1, &renderTarget, nullptr);
	context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(depthStencilState.get(), 0);
	context->RSSetState(rasterizerState.get());
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	ID3D11ShaderResourceView* particleSRV = particleBuffer->srv.get();
	context->VSSetConstantBuffers(0, 1, &frameBuffer);
	context->VSSetConstantBuffers(12, 1, &perFrame);
	context->VSSetShaderResources(0, 1, &particleSRV);
	context->VSSetShader(vertexShader, nullptr, 0);

	ID3D11ShaderResourceView* pixelResources[kPSResourceSlots] = { sceneDepth, shadowMask.SRV };
	context->PSSetConstantBuffers(0, 1, &frameBuffer);
	context->PSSetConstantBuffers(5, 2, sharedBuffers);
	context->PSSetConstantBuffers(12, 1, &perFrame);
	context->PSSetShaderResources(0, kPSResourceSlots, pixelResources);
	context->PSSetSamplers(0, 1, &sampler);
	context->PSSetShader(pixelShader, nullptr, 0);

	context->DrawInstanced(4, kMaxParticles, 0, 0);

	profiler->EndPass();

	backup.Restore(context);
	debug.renderedFrames++;
}

void FootstepParticles::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	ImGui::Checkbox(T(TKEY("include_npcs"), "Include NPCs and Creatures"), &settings.IncludeNPCs);

	ImGui::SliderFloat(T(TKEY("max_distance"), "Max Distance"), &settings.MaxDistance, 512.0f, 8192.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("max_distance_tooltip"), "Footsteps further than this from the camera (in game units) spawn nothing."));

	if (ImGui::TreeNodeEx(T(TKEY("surfaces"), "Surfaces"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("dust_intensity"), "Dust and Dirt"), &settings.DustIntensity, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("snow_intensity"), "Snow"), &settings.SnowIntensity, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("splash_intensity"), "Splashes"), &settings.SplashIntensity, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);

		ImGui::BeginDisabled(!globals::features::snowCover.loaded);
		ImGui::Checkbox(T(TKEY("use_snow_cover"), "Snow Cover Kicks Up Snow"), &settings.UseSnowCover);
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("use_snow_cover_tooltip"), "Ground covered by Snow Cover snow behaves like snow, not just vanilla snow textures."));

		ImGui::BeginDisabled(!globals::features::wetnessEffects.loaded);
		ImGui::Checkbox(T(TKEY("use_wetness_effects"), "Rain Puddles Splash"), &settings.UseWetnessEffects);
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("use_wetness_effects_tooltip"), "Splashes follow Wetness Effects puddles and rain wetness. Wet ground also stops dust."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("appearance"), "Appearance"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("particle_size"), "Particle Size"), &settings.ParticleSize, 0.25f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("lifetime"), "Lifetime"), &settings.Lifetime, 0.25f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("opacity"), "Opacity"), &settings.Opacity, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("debug"), "Debug"))) {
		int surfaceOverride = static_cast<int>(settings.SurfaceOverride);
		if (ImGui::Combo(T(TKEY("surface_override"), "Surface Override"), &surfaceOverride, kSurfaceNames.data(), static_cast<int>(kSurfaceNames.size())))
			settings.SurfaceOverride = static_cast<uint>(surfaceOverride);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextWrapped("%s", T(TKEY("surface_override_tooltip"), "Forces every footstep to use one surface type, for testing."));

		if (ImGui::Button(T(TKEY("test_burst"), "Spawn Test Footsteps")))
			testBurstQueued = true;

		ImGui::Text("%s: %u", T(TKEY("footsteps_per_second"), "Footsteps per second"), debug.eventsLastSecond);
		ImGui::Text("%s: %s (%s)", T(TKEY("player_surface"), "Player surface"), kSurfaceNames[static_cast<size_t>(debug.lastPlayerSurface) + 1], debug.lastPlayerMaterial.empty() ? "-" : debug.lastPlayerMaterial.c_str());
		ImGui::Text("%s: %llu", T(TKEY("rendered_frames"), "Rendered frames"), debug.renderedFrames);
		ImGui::TreePop();
	}
}

void FootstepParticles::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	sanitize(settings.MaxDistance, defaults.MaxDistance, 512.0f, 8192.0f);
	sanitize(settings.DustIntensity, defaults.DustIntensity, 0.0f, 3.0f);
	sanitize(settings.SnowIntensity, defaults.SnowIntensity, 0.0f, 3.0f);
	sanitize(settings.SplashIntensity, defaults.SplashIntensity, 0.0f, 3.0f);
	sanitize(settings.ParticleSize, defaults.ParticleSize, 0.25f, 3.0f);
	sanitize(settings.Lifetime, defaults.Lifetime, 0.25f, 2.0f);
	sanitize(settings.Opacity, defaults.Opacity, 0.0f, 2.0f);
	if (settings.SurfaceOverride > static_cast<uint>(Surface::Count))
		settings.SurfaceOverride = 0;
}

void FootstepParticles::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FootstepParticles::RestoreDefaultSettings()
{
	settings = {};
	clearRequested = true;
}

#undef I18N_KEY_PREFIX
