#include "HeatHaze.h"

#include "Deferred.h"
#include "Features/SnowCover.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.heat_haze."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	HeatHaze::Settings,
	Enabled,
	Strength,
	MaxDistance)

namespace
{
	constexpr float kScanInterval = 0.5f;
	constexpr uint32_t kMaxClassificationsPerScan = 64;
	constexpr float kMinRadius = 8.0f;
	constexpr float kMaxRadius = 64.0f;
	constexpr float kMaxSampleOffset = 128.0f;
	constexpr float kPlumeHeightScale = 7.0f;
	constexpr float kDistanceFadeShare = 0.35f;
	constexpr float kScreenMargin = 0.3f;
	constexpr float kMinDistance = 512.0f;
	constexpr float kMaxDistance = 6000.0f;
	constexpr float kMaxStrength = 3.0f;
	constexpr float kPlumeWidthScale = 1.6f;
	constexpr float kCopyBoundsPadding = 1.15f;
	constexpr float kMaxSampleOffsetUV = 0.006f;
	constexpr float kCopyMarginPixels = 2.0f;

	struct BSShaderAccumulator_RenderEffects
	{
		static void thunk(RE::BSShaderAccumulator* a_accumulator, uint32_t a_renderFlags)
		{
			globals::features::heatHaze.Render(a_accumulator);
			func(a_accumulator, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	bool IsNearScreen(RE::NiCamera* a_camera, const RE::NiPoint3& a_point)
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		return a_camera->WorldPtToScreenPt3(a_point, x, y, z, 1e-5f) && z > 0.0f &&
		       x > -kScreenMargin && x < 1.0f + kScreenMargin && y > -kScreenMargin && y < 1.0f + kScreenMargin;
	}

	class HazeStateBackup
	{
	public:
		explicit HazeStateBackup(ID3D11DeviceContext* a_context) :
			context(a_context)
		{
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, &depthStencilView);
			context->OMGetBlendState(&blendState, blendFactor, &sampleMask);
			context->OMGetDepthStencilState(&depthStencilState, &stencilRef);
			context->RSGetState(&rasterizerState);
			context->RSGetViewports(&viewportCount, viewports);
			context->IAGetInputLayout(&inputLayout);
			context->IAGetPrimitiveTopology(&topology);
			context->VSGetShader(&vertexShader, nullptr, nullptr);
			context->VSGetConstantBuffers(0, 1, &vsFrame);
			context->VSGetConstantBuffers(12, 1, &vsPerFrame);
			context->PSGetShader(&pixelShader, nullptr, nullptr);
			context->PSGetConstantBuffers(0, 1, &psFrame);
			context->PSGetConstantBuffers(5, 2, psShared);
			context->PSGetConstantBuffers(12, 1, &psPerFrame);
			context->PSGetShaderResources(0, 2, psResources);
			context->PSGetSamplers(0, 1, &psSampler);
		}

		HazeStateBackup(const HazeStateBackup&) = delete;
		HazeStateBackup& operator=(const HazeStateBackup&) = delete;

		~HazeStateBackup()
		{
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, renderTargets, depthStencilView);
			context->OMSetBlendState(blendState, blendFactor, sampleMask);
			context->OMSetDepthStencilState(depthStencilState, stencilRef);
			context->RSSetState(rasterizerState);
			context->RSSetViewports(viewportCount, viewports);
			context->IASetInputLayout(inputLayout);
			context->IASetPrimitiveTopology(topology);
			context->VSSetShader(vertexShader, nullptr, 0);
			context->VSSetConstantBuffers(0, 1, &vsFrame);
			context->VSSetConstantBuffers(12, 1, &vsPerFrame);
			context->PSSetShader(pixelShader, nullptr, 0);
			context->PSSetConstantBuffers(0, 1, &psFrame);
			context->PSSetConstantBuffers(5, 2, psShared);
			context->PSSetConstantBuffers(12, 1, &psPerFrame);
			context->PSSetShaderResources(0, 2, psResources);
			context->PSSetSamplers(0, 1, &psSampler);

			for (auto* view : renderTargets)
				SafeRelease(view);
			SafeRelease(depthStencilView);
			SafeRelease(blendState);
			SafeRelease(depthStencilState);
			SafeRelease(rasterizerState);
			SafeRelease(inputLayout);
			SafeRelease(vertexShader);
			SafeRelease(vsFrame);
			SafeRelease(vsPerFrame);
			SafeRelease(pixelShader);
			SafeRelease(psFrame);
			for (auto* buffer : psShared)
				SafeRelease(buffer);
			SafeRelease(psPerFrame);
			for (auto* view : psResources)
				SafeRelease(view);
			SafeRelease(psSampler);
		}

	private:
		template <class T>
		static void SafeRelease(T* a_object)
		{
			if (a_object)
				a_object->Release();
		}

		ID3D11DeviceContext* context;
		ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* depthStencilView = nullptr;
		ID3D11BlendState* blendState = nullptr;
		FLOAT blendFactor[4] = {};
		UINT sampleMask = 0;
		ID3D11DepthStencilState* depthStencilState = nullptr;
		UINT stencilRef = 0;
		ID3D11RasterizerState* rasterizerState = nullptr;
		UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
		ID3D11InputLayout* inputLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11VertexShader* vertexShader = nullptr;
		ID3D11Buffer* vsFrame = nullptr;
		ID3D11Buffer* vsPerFrame = nullptr;
		ID3D11PixelShader* pixelShader = nullptr;
		ID3D11Buffer* psFrame = nullptr;
		ID3D11Buffer* psShared[2] = {};
		ID3D11Buffer* psPerFrame = nullptr;
		ID3D11ShaderResourceView* psResources[2] = {};
		ID3D11SamplerState* psSampler = nullptr;
	};
}

void HeatHaze::SetupResources()
{
	auto device = globals::d3d::device;

	frameConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<FrameData>(), "HeatHaze::FrameData");

	{
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
		Util::SetResourceName(linearSampler.get(), "HeatHaze::LinearSampler");
	}

	{
		D3D11_BLEND_DESC blendDesc{};
		auto& target = blendDesc.RenderTarget[0];
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
		target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		target.BlendOp = D3D11_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D11_BLEND_ZERO;
		target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, blendState.put()));
		Util::SetResourceName(blendState.get(), "HeatHaze::BlendState");
	}

	{
		D3D11_RASTERIZER_DESC rasterizerDesc{};
		rasterizerDesc.FillMode = D3D11_FILL_SOLID;
		rasterizerDesc.CullMode = D3D11_CULL_NONE;
		rasterizerDesc.DepthClipEnable = TRUE;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put()));
		Util::SetResourceName(rasterizerState.get(), "HeatHaze::RasterizerState");
	}

	{
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = FALSE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthStencilState.put()));
		Util::SetResourceName(depthStencilState.get(), "HeatHaze::DepthStencilState");
	}
}

void HeatHaze::ClearShaderCache()
{
	vertexShader = nullptr;
	pixelShader = nullptr;
	shaderCompileFailed = false;
}

ID3D11VertexShader* HeatHaze::GetVS()
{
	if (!vertexShader && !shaderCompileFailed) {
		vertexShader.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\HeatHaze\\HeatHazeVS.hlsl", {}, "vs_5_0")));
		shaderCompileFailed = !vertexShader;
	}
	return vertexShader.get();
}

ID3D11PixelShader* HeatHaze::GetPS()
{
	if (!pixelShader && !shaderCompileFailed) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", nullptr });
		pixelShader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\HeatHaze\\HeatHazePS.hlsl", defines, "ps_5_0")));
		shaderCompileFailed = !pixelShader;
	}
	return pixelShader.get();
}

void HeatHaze::PostPostLoad()
{
	stl::detour_thunk<BSShaderAccumulator_RenderEffects>(REL::RelocationID(99940, 106585));
	logger::info("[HeatHaze] Installed hooks");
}

void HeatHaze::ScanFires()
{
	fires.clear();

	auto* tes = RE::TES::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!tes || !player)
		return;

	uint32_t classified = 0;
	tes->ForEachReferenceInRange(player, settings.MaxDistance, [&](RE::TESObjectREFR* a_ref) {
		if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted())
			return RE::BSContainer::ForEachResult::kContinue;
		auto* base = a_ref->GetBaseObject();
		if (!SnowCover::IsWorldFireSource(base))
			return RE::BSContainer::ForEachResult::kContinue;
		const auto cached = fireBaseCache.find(base->GetFormID());
		const bool known = cached != fireBaseCache.end();
		if ((known && !cached->second) || (!known && classified >= kMaxClassificationsPerScan))
			return RE::BSContainer::ForEachResult::kContinue;
		auto* root = a_ref->Get3D();
		if (!root)
			return RE::BSContainer::ForEachResult::kContinue;

		bool hasFire = false;
		fireSamples.clear();
		SnowCover::CollectFireGeometry(root, false, hasFire, fireSamples);
		if (!known) {
			fireBaseCache.emplace(base->GetFormID(), hasFire);
			++classified;
		}

		const auto refPosition = a_ref->GetPosition();
		constexpr float kUnbounded = std::numeric_limits<float>::max();
		RE::NiPoint3 boundsMin{ kUnbounded, kUnbounded, kUnbounded };
		RE::NiPoint3 boundsMax{ -kUnbounded, -kUnbounded, -kUnbounded };
		for (const auto& sample : fireSamples) {
			if (sample.center.GetSquaredDistance(refPosition) > kMaxSampleOffset * kMaxSampleOffset)
				continue;
			boundsMin.x = std::min(boundsMin.x, sample.center.x - sample.radius);
			boundsMin.y = std::min(boundsMin.y, sample.center.y - sample.radius);
			boundsMin.z = std::min(boundsMin.z, sample.center.z);
			boundsMax.x = std::max(boundsMax.x, sample.center.x + sample.radius);
			boundsMax.y = std::max(boundsMax.y, sample.center.y + sample.radius);
			boundsMax.z = std::max(boundsMax.z, sample.center.z);
		}
		if (boundsMin.x > boundsMax.x)
			return RE::BSContainer::ForEachResult::kContinue;

		Fire fire;
		fire.center = { (boundsMin.x + boundsMax.x) * 0.5f, (boundsMin.y + boundsMax.y) * 0.5f, boundsMax.z };
		fire.radius = std::clamp(0.5f * std::max(boundsMax.x - boundsMin.x, boundsMax.y - boundsMin.y), kMinRadius, kMaxRadius);
		const uint32_t formID = a_ref->GetFormID();
		fire.seed = static_cast<float>((formID * 2654435761u) >> 20) / 4096.0f;
		fires.push_back(fire);
		return RE::BSContainer::ForEachResult::kContinue;
	});
}

void HeatHaze::SelectPlumes()
{
	auto* camera = RE::Main::WorldRootCamera();
	if (!camera)
		return;

	const RE::NiPoint3 eye = Util::GetEyePosition();
	const float maxDistance = settings.MaxDistance;

	fireOrder.clear();
	for (uint32_t i = 0; i < fires.size(); ++i) {
		const auto& fire = fires[i];
		const float distance = eye.GetDistance(fire.center);
		if (distance >= maxDistance)
			continue;
		const RE::NiPoint3 top = fire.center + RE::NiPoint3{ 0.0f, 0.0f, fire.radius * kPlumeHeightScale };
		if (distance < fire.radius * 4.0f || IsNearScreen(camera, fire.center) || IsNearScreen(camera, top))
			fireOrder.emplace_back(distance, i);
	}

	const size_t count = std::min<size_t>(fireOrder.size(), kMaxPlumes);
	std::partial_sort(fireOrder.begin(), fireOrder.begin() + count, fireOrder.end());

	for (size_t i = 0; i < count; ++i) {
		const auto [distance, index] = fireOrder[i];
		const auto& fire = fires[index];
		frameData.Plumes[i] = float4{ fire.center.x, fire.center.y, fire.center.z, fire.radius };
		const float fade = std::clamp((maxDistance - distance) / (maxDistance * kDistanceFadeShare), 0.0f, 1.0f);
		frameData.PlumeParams[i] = float4{ fade, fire.seed, 0.0f, 0.0f };
	}
	plumeCount = static_cast<uint32_t>(count);
}

float HeatHaze::GetNearestFireDistance() const
{
	if (!loaded || !settings.Enabled || fires.empty())
		return std::numeric_limits<float>::max();
	const RE::NiPoint3 eye = Util::GetEyePosition();
	float nearest = std::numeric_limits<float>::max();
	for (const auto& fire : fires)
		nearest = std::min(nearest, eye.GetDistance(fire.center) - fire.radius);
	return std::max(nearest, 0.0f);
}

void HeatHaze::Prepass()
{
	plumeCount = 0;
	if (!settings.Enabled || settings.Strength <= 0.0f || globals::state->isLoadingMenuOpen) {
		fires.clear();
		scanTimer = kScanInterval;
		return;
	}

	const float frameTime = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f);
	const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
	if (!paused)
		// The shimmer scrolls at Time * 1.1 (and 2.3x that for the fine octave) through value noise that repeats every
		// 256 cells. Wrap where both octaves have moved a whole number of periods (10 and 23), or every plume's
		// pattern snaps to an unrelated one on the wrap frame.
		time = std::fmod(time + frameTime, 2560.0f / 1.1f);

	scanTimer += frameTime;
	if (scanTimer >= kScanInterval) {
		scanTimer = 0.0f;
		ScanFires();
	}

	if (!fires.empty())
		SelectPlumes();
}

bool HeatHaze::BuildCopyBoxes(const float2& a_resolution, const float2& a_size)
{
	copyBoxes.clear();

	auto* camera = RE::Main::WorldRootCamera();
	if (!camera)
		return false;

	const RE::NiPoint3 eye = Util::GetEyePosition();
	const float marginX = std::ceil(kMaxSampleOffsetUV * a_size.x) + kCopyMarginPixels;
	const float marginY = std::ceil(kMaxSampleOffsetUV * a_size.y) + kCopyMarginPixels;

	for (uint32_t i = 0; i < plumeCount; ++i) {
		const auto& plume = frameData.Plumes[i];
		const RE::NiPoint3 center{ plume.x, plume.y, plume.z };
		const RE::NiPoint3 toPlume = center - eye;
		RE::NiPoint3 side{ toPlume.y, -toPlume.x, 0.0f };
		const float sideLength = side.Length();
		if (sideLength < 1e-3f)
			return false;
		side *= plume.w * kPlumeWidthScale * kCopyBoundsPadding / sideLength;
		const RE::NiPoint3 up{ 0.0f, 0.0f, plume.w * kPlumeHeightScale * kCopyBoundsPadding };

		float minX = std::numeric_limits<float>::max();
		float minY = std::numeric_limits<float>::max();
		float maxX = std::numeric_limits<float>::lowest();
		float maxY = std::numeric_limits<float>::lowest();
		const RE::NiPoint3 corners[4] = { center - side, center + side, center - side + up, center + side + up };
		for (const auto& corner : corners) {
			float x = 0.0f;
			float y = 0.0f;
			float z = 0.0f;
			if (!camera->WorldPtToScreenPt3(corner, x, y, z, 1e-5f) || z <= 0.0f)
				return false;
			minX = std::min(minX, x);
			maxX = std::max(maxX, x);
			minY = std::min(minY, 1.0f - y);
			maxY = std::max(maxY, 1.0f - y);
		}

		const float left = std::clamp(std::floor(minX * a_resolution.x) - marginX, 0.0f, a_size.x);
		const float right = std::clamp(std::ceil(maxX * a_resolution.x) + marginX, 0.0f, a_size.x);
		const float top = std::clamp(std::floor(minY * a_resolution.y) - marginY, 0.0f, a_size.y);
		const float bottom = std::clamp(std::ceil(maxY * a_resolution.y) + marginY, 0.0f, a_size.y);
		if (right <= left || bottom <= top)
			continue;

		D3D11_BOX box{};
		box.left = static_cast<UINT>(left);
		box.top = static_cast<UINT>(top);
		box.right = static_cast<UINT>(right);
		box.bottom = static_cast<UINT>(bottom);
		box.front = 0;
		box.back = 1;
		copyBoxes.push_back(box);
	}
	return true;
}

bool HeatHaze::EnsureSceneCopy(const D3D11_TEXTURE2D_DESC& a_desc)
{
	if (sceneCopy && sceneCopy->desc.Width == a_desc.Width && sceneCopy->desc.Height == a_desc.Height && sceneCopy->desc.Format == a_desc.Format)
		return true;

	sceneCopy.reset();

	D3D11_TEXTURE2D_DESC desc = a_desc;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	try {
		sceneCopy = eastl::make_unique<Texture2D>(desc, "HeatHaze::SceneCopy");
		sceneCopy->CreateSRV(srvDesc);
	} catch (const std::exception& e) {
		logger::error("[HeatHaze] Failed to create the scene copy ({}), turning Heat Haze off", e.what());
		sceneCopy.reset();
		settings.Enabled = false;
		return false;
	}
	return true;
}

void HeatHaze::Render(RE::BSShaderAccumulator* a_accumulator)
{
	if (!loaded || !a_accumulator || plumeCount == 0)
		return;

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

	Draw();
}

void HeatHaze::Draw()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	if (!context || !renderer || !frameConstants)
		return;

	auto vs = GetVS();
	auto ps = GetPS();
	if (!vs || !ps)
		return;

	auto mainTarget = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[0]];
	auto sceneDepth = Util::GetCurrentSceneDepthSRV(false);
	if (!mainTarget.RTV || !mainTarget.texture || !sceneDepth)
		return;

	D3D11_TEXTURE2D_DESC textureDesc{};
	mainTarget.texture->GetDesc(&textureDesc);
	if (!EnsureSceneCopy(textureDesc))
		return;

	const float2 size{ static_cast<float>(textureDesc.Width), static_cast<float>(textureDesc.Height) };
	const float2 resolution = Util::ConvertToDynamic(size);

	frameData.UVMax = { std::max(resolution.x - 1.0f, 0.5f) / size.x, std::max(resolution.y - 1.0f, 0.5f) / size.y };
	frameData.Time = time;
	frameData.Strength = settings.Strength;
	frameConstants->Update(frameData);

	const bool partialCopy = BuildCopyBoxes(resolution, size);
	if (partialCopy && copyBoxes.empty())
		return;

	HazeStateBackup backup(context);

	auto profiler = globals::profiler;
	profiler->BeginPass("HeatHaze");

	context->OMSetRenderTargets(0, nullptr, nullptr);
	if (partialCopy) {
		for (const auto& box : copyBoxes)
			context->CopySubresourceRegion(sceneCopy->resource.get(), 0, box.left, box.top, 0, mainTarget.texture, 0, &box);
	} else {
		context->CopyResource(sceneCopy->resource.get(), mainTarget.texture);
	}

	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, resolution.x, resolution.y, 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);

	ID3D11RenderTargetView* renderTarget = mainTarget.RTV;
	context->OMSetRenderTargets(1, &renderTarget, nullptr);
	context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(depthStencilState.get(), 0);
	context->RSSetState(rasterizerState.get());
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	ID3D11Buffer* frameBuffer = frameConstants->CB();
	ID3D11Buffer* perFrame = *globals::game::perFrame.get();
	ID3D11Buffer* sharedBuffers[2] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	ID3D11SamplerState* sampler = linearSampler.get();
	ID3D11ShaderResourceView* resources[2] = { sceneCopy->srv.get(), sceneDepth };

	context->VSSetConstantBuffers(0, 1, &frameBuffer);
	context->VSSetConstantBuffers(12, 1, &perFrame);
	context->VSSetShader(vs, nullptr, 0);
	context->PSSetConstantBuffers(0, 1, &frameBuffer);
	context->PSSetConstantBuffers(5, 2, sharedBuffers);
	context->PSSetConstantBuffers(12, 1, &perFrame);
	context->PSSetShaderResources(0, 2, resources);
	context->PSSetSamplers(0, 1, &sampler);
	context->PSSetShader(ps, nullptr, 0);

	context->DrawInstanced(4, plumeCount, 0, 0);

	profiler->EndPass();
}

void HeatHaze::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);

	ImGui::SliderFloat(T(TKEY("strength"), "Strength"), &settings.Strength, 0.0f, kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("strength_tooltip"), "How far the hot air bends what is behind it."));

	ImGui::SliderFloat(T(TKEY("max_distance"), "Max Distance"), &settings.MaxDistance, kMinDistance, kMaxDistance, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextWrapped("%s", T(TKEY("max_distance_tooltip"), "Fires further than this from the player (in game units) have no haze."));
}

void HeatHaze::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	sanitize(settings.Strength, defaults.Strength, 0.0f, kMaxStrength);
	sanitize(settings.MaxDistance, defaults.MaxDistance, kMinDistance, kMaxDistance);
}

void HeatHaze::SaveSettings(json& o_json)
{
	o_json = settings;
}

void HeatHaze::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
