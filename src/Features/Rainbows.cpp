#include "Rainbows.h"

#include "Deferred.h"
#include "Features/Effects11/D3D11StateBackup.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/PhysicalSky.h"
#include "Features/Rainbows/RainbowOptics.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/DebugSection.h"
#include "Utils/Game.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.rainbows."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Rainbows::Settings,
	Enabled,
	Moonbows,
	Strength,
	SecondaryBow,
	InnerGlow,
	DropSize,
	Chance,
	Linger,
	FogAbsorption)

namespace
{
	constexpr float kMaxStrength = 4.0f;
	constexpr float kMinDropSize = 10.0f;
	constexpr float kMaxDropSize = 1500.0f;
	constexpr float kMaxLinger = 120.0f;

	// Peak bow radiance at Strength 1 through a full rain curtain, as the albedo of a sunlit Lambert surface.
	constexpr float kPeakAlbedo = 0.25f;
	// The shower: rain falls from a cloud base this far above the camera, out to this distance, and this much
	// rain path (1/units) scatters about 63% of what a full curtain would. Game units, 70 per meter.
	constexpr float kRainHeight = 70000.0f;
	constexpr float kRainExtent = 350000.0f;
	constexpr float kRainDensity = 1.0f / 105000.0f;
	constexpr float kDepartedRainStart = 35000.0f;

	constexpr float kShowerStart = 0.05f;
	constexpr float kShowerEnd = 0.01f;
	constexpr float kFadeSeconds = 2.0f;
	constexpr float kMaxGameHourJump = 1.0f;

	struct BSShaderAccumulator_RenderEffects
	{
		static void thunk(RE::BSShaderAccumulator* a_accumulator, uint32_t a_renderFlags)
		{
			globals::features::rainbows.Render(a_accumulator);
			func(a_accumulator, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	float Linearstep(float a_edge0, float a_edge1, float a_x)
	{
		if (a_edge1 <= a_edge0)
			return a_x >= a_edge1 ? 1.0f : 0.0f;
		return std::clamp((a_x - a_edge0) / (a_edge1 - a_edge0), 0.0f, 1.0f);
	}

	/** Rain falling in a weather, 0 to 1: drizzle (particle density 1) is fainter than a downpour (2 and up). */
	float WeatherRain(const RE::TESWeather* a_weather)
	{
		if (!a_weather || !a_weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy))
			return 0.0f;
		float density = 0.0f;
		if (a_weather->precipitationData)
			density = a_weather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
		if (!std::isfinite(density) || density <= 0.0f)
			return 0.6f;
		return std::clamp(density * 0.5f, 0.4f, 1.0f);
	}

	/** How freely sunlight reaches the rain under a weather's own sky. */
	float WeatherSunshine(const RE::TESWeather* a_weather)
	{
		using Flag = RE::TESWeather::WeatherDataFlag;
		if (!a_weather)
			return 0.0f;
		const auto& flags = a_weather->data.flags;
		if (flags.any(Flag::kRainy, Flag::kSnow))
			return 0.0f;
		if (flags.any(Flag::kCloudy))
			return 0.6f;
		if (flags.any(Flag::kPleasant))
			return 1.0f;
		return 0.8f;
	}
}

namespace
{
	constexpr float kReferenceDrop = 500.0f;
	constexpr float kReferenceSecondary = 0.43f;
	constexpr float kReferenceGlow = 0.35f;

	/** Peak luminance of the default bow, so Brightness means the same whatever the drop size. */
	float ReferencePeak()
	{
		static const float peak = [] {
			std::vector<RainbowOptics::RGB> profile;
			const float value = RainbowOptics::BuildProfile(kReferenceDrop, kReferenceSecondary, kReferenceGlow, Rainbows::kLutSize, Rainbows::kLutMaxDegrees, profile);
			return std::max(value, 1e-6f);
		}();
		return peak;
	}
}

void Rainbows::SetupResources()
{
	auto device = globals::d3d::device;

	frameConstants = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<FrameData>(), "Rainbows::FrameData");

	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kLutSize;
		desc.Height = 1;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		lut = eastl::make_unique<Texture2D>(desc, "Rainbows::BowLUT");
		lut->CreateSRV(srvDesc);
	}

	{
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
		Util::SetResourceName(linearSampler.get(), "Rainbows::LinearSampler");
	}

	{
		D3D11_BLEND_DESC blendDesc{};
		auto& target = blendDesc.RenderTarget[0];
		target.BlendEnable = TRUE;
		target.SrcBlend = D3D11_BLEND_ONE;
		target.DestBlend = D3D11_BLEND_ONE;
		target.BlendOp = D3D11_BLEND_OP_ADD;
		target.SrcBlendAlpha = D3D11_BLEND_ZERO;
		target.DestBlendAlpha = D3D11_BLEND_ONE;
		target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, blendState.put()));
		Util::SetResourceName(blendState.get(), "Rainbows::BlendState");
	}

	{
		D3D11_RASTERIZER_DESC rasterizerDesc{};
		rasterizerDesc.FillMode = D3D11_FILL_SOLID;
		rasterizerDesc.CullMode = D3D11_CULL_NONE;
		rasterizerDesc.DepthClipEnable = FALSE;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put()));
		Util::SetResourceName(rasterizerState.get(), "Rainbows::RasterizerState");
	}

	{
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = FALSE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthStencilState.put()));
		Util::SetResourceName(depthStencilState.get(), "Rainbows::DepthStencilState");
	}
}

void Rainbows::ClearShaderCache()
{
	vertexShader = nullptr;
	pixelShader = nullptr;
	shaderCompileFailed = false;
}

ID3D11VertexShader* Rainbows::GetVS()
{
	if (!vertexShader && !shaderCompileFailed) {
		vertexShader.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\Rainbows\\RainbowsVS.hlsl", {}, "vs_5_0")));
		shaderCompileFailed = !vertexShader;
	}
	return vertexShader.get();
}

ID3D11PixelShader* Rainbows::GetPS()
{
	if (!pixelShader && !shaderCompileFailed) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", nullptr });
		if (globals::features::physicalSky.loaded)
			defines.push_back({ "PHYSICAL_SKY", nullptr });
		if (globals::features::exponentialHeightFog.loaded)
			defines.push_back({ "EXP_HEIGHT_FOG", nullptr });
		pixelShader.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\Rainbows\\RainbowsPS.hlsl", defines, "ps_5_0")));
		shaderCompileFailed = !pixelShader;
	}
	return pixelShader.get();
}

void Rainbows::PostPostLoad()
{
	stl::detour_thunk<BSShaderAccumulator_RenderEffects>(REL::RelocationID(99940, 106585));
	logger::info("[Rainbows] Installed hooks");
}

void Rainbows::EndShower()
{
	showerActive = false;
	showerHasBow = false;
	showerRain = 0.0f;
}

void Rainbows::UpdateWeather(float a_deltaTime)
{
	auto* sky = globals::game::sky;
	auto* calendar = RE::Calendar::GetSingleton();
	const bool exterior = sky && sky->mode.get() == RE::Sky::Mode::kFull && !sky->flags.any(RE::Sky::Flags::kHideSky);

	float rain = 0.0f;
	float sunshine = 0.0f;
	if (exterior) {
		const float pct = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
		if (const auto* incoming = sky->currentWeather)
			rain += WeatherRain(incoming) * Linearstep(incoming->data.precipitationBeginFadeIn / 255.0f, 1.0f, pct);
		if (const auto* outgoing = sky->lastWeather; outgoing && outgoing != sky->currentWeather)
			rain += WeatherRain(outgoing) * (1.0f - Linearstep(0.0f, outgoing->data.precipitationEndFadeOut / 255.0f, pct));
		rain = std::min(rain, 1.0f);
		sunshine = sky->lastWeather ? std::lerp(WeatherSunshine(sky->lastWeather), WeatherSunshine(sky->currentWeather), pct) : WeatherSunshine(sky->currentWeather);
	}
	debug.rain = rain;
	debug.sunshine = sunshine;

	// Game time drives the lingering, so it keeps pace with the timescale. A load, wait, sleep or fast travel
	// jumps the clock, and any shower from before the jump is gone.
	const float hours = calendar ? calendar->GetHoursPassed() : 0.0f;
	const float elapsedHours = lastGameHours < 0.0f ? 0.0f : hours - lastGameHours;
	lastGameHours = hours;
	if (elapsedHours < 0.0f || elapsedHours > kMaxGameHourJump || !exterior)
		EndShower();

	// Once the rain stops falling here it keeps falling where the shower moved off to, thinning over Linger.
	const float decay = settings.Linger > 0.0f ? std::exp(-std::max(elapsedHours, 0.0f) * 60.0f * 3.0f / settings.Linger) : 0.0f;
	showerRain = std::max(rain, showerRain * decay);

	if (!showerActive && rain > kShowerStart) {
		showerActive = true;
		showerHasBow = std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < settings.Chance;
	}
	if (showerActive && showerRain < kShowerEnd)
		EndShower();

	const float rainHere = showerRain > 0.0f ? std::clamp(rain / showerRain, 0.0f, 1.0f) : 0.0f;
	rainStart = (1.0f - rainHere) * kDepartedRainStart;

	if (!exterior) {
		visibility = 0.0f;
		return;
	}
	if (preview) {
		visibility = 1.0f;
		return;
	}

	const float target = showerHasBow ? showerRain * sunshine : 0.0f;
	visibility += (target - visibility) * (1.0f - std::exp(-a_deltaTime / kFadeSeconds));
	if (visibility < 1e-4f && target <= 0.0f)
		visibility = 0.0f;
}

void Rainbows::Prepass()
{
	if (!settings.Enabled || globals::state->isLoadingMenuOpen) {
		visibility = 0.0f;
		lastGameHours = -1.0f;
		EndShower();
		return;
	}
	// Built here rather than on the first frame that draws a bow, where the 5-20 ms would show as a hitch.
	BuildLut();
	UpdateWeather(std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f));
}

void Rainbows::BuildLut()
{
	const LutKey key{ settings.DropSize, settings.SecondaryBow, settings.InnerGlow };
	if (key == lutKey || !lut)
		return;

	std::vector<RainbowOptics::RGB> profile;
	RainbowOptics::BuildProfile(settings.DropSize, settings.SecondaryBow, settings.InnerGlow, kLutSize, kLutMaxDegrees, profile);
	const float scale = 1.0f / ReferencePeak();
	std::vector<float4> texels(profile.size());
	for (size_t i = 0; i < profile.size(); ++i)
		texels[i] = float4{ profile[i].r * scale, profile[i].g * scale, profile[i].b * scale, 0.0f };

	globals::d3d::context->UpdateSubresource(lut->resource.get(), 0, nullptr, texels.data(), static_cast<UINT>(texels.size() * sizeof(float4)), 0);
	lutKey = key;
}

void Rainbows::Render(RE::BSShaderAccumulator* a_accumulator)
{
	if (!loaded || !a_accumulator || !settings.Enabled || settings.Strength <= 0.0f || visibility <= 0.0f)
		return;

	auto state = globals::state;
	if (!state || !state->inWorld || state->isMapMenuOpen || !globals::shaderCache->IsEnabled())
		return;
	if (auto water = RE::TESWaterSystem::GetSingleton(); water && water->playerUnderwater)
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

void Rainbows::Draw()
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	if (!context || !renderer || !frameConstants || !lut || lutKey.dropSize < 0.0f)
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
	const float2 resolution = Util::ConvertToDynamic(float2{ static_cast<float>(textureDesc.Width), static_cast<float>(textureDesc.Height) });

	FrameData frameData{};
	if (auto smState = globals::game::smState) {
		if (auto sceneNode = smState->shadowSceneNode[0]) {
			if (auto& fog = sceneNode->GetRuntimeData().fogProperty) {
				const float range = fog->farDistance - fog->nearDistance;
				if (range > 1.0f && std::isfinite(range))
					frameData.FogParams = { fog->nearDistance / range, 1.0f / range, std::max(fog->power, 0.0f), std::clamp(fog->clamp, 0.0f, 1.0f) };
			}
		}
	}
	frameData.RenderSize = resolution;
	frameData.Intensity = settings.Strength * kPeakAlbedo * visibility;
	frameData.FogAbsorption = settings.FogAbsorption;
	frameData.MoonIntensity = settings.Moonbows ? 1.0f : 0.0f;
	frameData.AngleToLut = 180.0f / (kLutMaxDegrees * std::numbers::pi_v<float>);
	frameData.RainHeight = kRainHeight;
	frameData.RainExtent = kRainExtent;
	frameData.RainDensity = kRainDensity;
	frameData.RainStart = rainStart;
	frameConstants->Update(frameData);

	Effects11Util::D3D11FullStateBackup backup;
	backup.Save(context);

	auto profiler = globals::profiler;
	profiler->BeginPass("Rainbows");

	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, resolution.x, resolution.y, 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);

	ID3D11RenderTargetView* renderTarget = mainTarget.RTV;
	context->OMSetRenderTargets(1, &renderTarget, nullptr);
	context->OMSetBlendState(blendState.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(depthStencilState.get(), 0);
	context->RSSetState(rasterizerState.get());
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	ID3D11Buffer* frameBuffer = frameConstants->CB();
	ID3D11Buffer* perFrame = *globals::game::perFrame.get();
	ID3D11Buffer* sharedBuffers[2] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	ID3D11ShaderResourceView* resources[2] = { sceneDepth, lut->srv.get() };
	ID3D11SamplerState* sampler = linearSampler.get();

	context->VSSetShader(vs, nullptr, 0);
	context->VSSetConstantBuffers(12, 1, &perFrame);
	context->PSSetConstantBuffers(0, 1, &frameBuffer);
	context->PSSetConstantBuffers(5, 2, sharedBuffers);
	context->PSSetConstantBuffers(12, 1, &perFrame);
	context->PSSetShaderResources(0, 2, resources);
	context->PSSetSamplers(0, 1, &sampler);
	context->PSSetShader(ps, nullptr, 0);

	context->Draw(3, 0);

	profiler->EndPass();
	backup.Restore(context);

	debug.renderedFrames++;
}

void Rainbows::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	Util::HoverTip(T(TKEY("enabled_tooltip"), "Rainbows form when sunlight falls on rain: as a shower passes, opposite the sun, and only outdoors."));

	ImGui::SliderFloat(T(TKEY("strength"), "Brightness"), &settings.Strength, 0.0f, kMaxStrength, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	Util::HoverTip(T(TKEY("strength_tooltip"), "How bright the bow is against the rain behind it."));

	ImGui::Checkbox(T(TKEY("moonbows"), "Moonbows"), &settings.Moonbows);
	Util::HoverTip(T(TKEY("moonbows_tooltip"), "At night, bright moonlight on rain makes a faint, nearly white bow, opposite the moon that lights the scene. Needs Sky Sync's moonlight to come from a moon."));

	if (ImGui::TreeNodeEx(T(TKEY("appearance"), "Appearance"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("drop_size"), "Drop Size"), &settings.DropSize, kMinDropSize, kMaxDropSize, "%.0f um", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		Util::HoverTip(T(TKEY("drop_size_tooltip"), "Raindrop radius in micrometers. Large drops give a narrow, vivid bow. Small drops add pastel fringes inside it, and mist-sized drops below about 30 fade it to a broad, white fogbow."));

		ImGui::SliderFloat(T(TKEY("secondary_bow"), "Secondary Bow"), &settings.SecondaryBow, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("secondary_bow_tooltip"), "Brightness of the outer bow, whose colors run in reverse, relative to the main bow. Real ones are a little under half as bright. 0 turns it off."));

		ImGui::SliderFloat(T(TKEY("inner_glow"), "Inner Glow"), &settings.InnerGlow, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("inner_glow_tooltip"), "The sky inside a rainbow is brighter than outside it, leaving a dark band between the two bows. 1 is physically correct."));

		ImGui::SliderFloat(T(TKEY("fog_absorption"), "Fog Absorption"), &settings.FogAbsorption, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("fog_absorption_tooltip"), "How much weather fog dims the bow."));
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("showers"), "Showers"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("chance"), "Chance per Shower"), &settings.Chance, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("chance_tooltip"), "Chance that a shower leaves a rainbow once the sun comes out. Rolled when the rain starts."));

		ImGui::SliderFloat(T(TKEY("linger"), "Linger"), &settings.Linger, 0.0f, kMaxLinger, "%.0f min", ImGuiSliderFlags_AlwaysClamp);
		Util::HoverTip(T(TKEY("linger_tooltip"), "Game minutes the bow stays in the departing rain after it stops falling where you are. It fades out over this time."));
		ImGui::TreePop();
	}
}

void Rainbows::DrawDebugSection()
{
	Util::DebugSection section;
	if (!section)
		return;

	ImGui::Checkbox(T(TKEY("preview"), "Preview"), &preview);
	Util::HoverTip(T(TKEY("preview_tooltip"), "Shows the bow now, in any weather, while checked. Outdoors only, and only where the sun or moon is low enough for it to be in view."));

	if (Util::Button(T(TKEY("roll_again"), "Roll Shower Again")))
		showerHasBow = showerActive && std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < settings.Chance;

	ImGui::Text("%s: %.2f", T(TKEY("debug_rain"), "Rain here"), debug.rain);
	ImGui::Text("%s: %.2f", T(TKEY("debug_shower"), "Shower rain"), showerRain);
	ImGui::Text("%s: %.2f", T(TKEY("debug_sunshine"), "Sunshine"), debug.sunshine);
	ImGui::Text("%s: %s", T(TKEY("debug_has_bow"), "Shower has a bow"), showerActive ? (showerHasBow ? T(TKEY("yes"), "Yes") : T(TKEY("no"), "No")) : "-");
	ImGui::Text("%s: %.3f", T(TKEY("debug_visibility"), "Visibility"), visibility);
	ImGui::Text("%s: %llu", T(TKEY("rendered_frames"), "Rendered frames"), debug.renderedFrames);
}

void Rainbows::LoadSettings(json& o_json)
{
	settings = o_json;
	const Settings defaults{};
	auto sanitize = [](float& a_value, float a_default, float a_min, float a_max) {
		a_value = std::clamp(std::isfinite(a_value) ? a_value : a_default, a_min, a_max);
	};
	sanitize(settings.Strength, defaults.Strength, 0.0f, kMaxStrength);
	sanitize(settings.SecondaryBow, defaults.SecondaryBow, 0.0f, 1.0f);
	sanitize(settings.InnerGlow, defaults.InnerGlow, 0.0f, 1.0f);
	sanitize(settings.DropSize, defaults.DropSize, kMinDropSize, kMaxDropSize);
	sanitize(settings.Chance, defaults.Chance, 0.0f, 1.0f);
	sanitize(settings.Linger, defaults.Linger, 0.0f, kMaxLinger);
	sanitize(settings.FogAbsorption, defaults.FogAbsorption, 0.0f, 1.0f);
}

void Rainbows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void Rainbows::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
