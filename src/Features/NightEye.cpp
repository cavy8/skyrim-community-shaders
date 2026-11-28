// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from Truly Night Eye 1.0 by Aswordmaster.
#include "NightEye.h"

#include <DDSTextureLoader.h>

#include "Features/CameraFX.h"
#include "Features/HDRDisplay.h"
#include "Features/PostProcessing/ImageSpaceModifiers.h"
#include "I18n/I18n.h"
#include "Profiler.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.night_eye."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NightEye::Settings,
	Enabled,
	Exposure,
	Gamma,
	ShadowLift,
	TintColor,
	TintAmount,
	Glow,
	Vignette,
	Grain,
	TransitionSeconds,
	EyeshineAmount,
	EyeshineColor,
	CustomVFX,
	CustomAmount,
	AnaglyphEnabled,
	AnaglyphAmount,
	AnaglyphHorizontal,
	AnaglyphVertical,
	AnaglyphDesaturation,
	AnaglyphLeftColor,
	AnaglyphRightColor,
	AnaglyphSwap,
	VisionMode,
	ActorVisionAmount,
	ActorVisionBrightness,
	EnemyColor,
	AllyColor,
	NeutralColor);

namespace
{
	struct Range
	{
		float minimum;
		float maximum;
	};
	constexpr Range kUnit{ 0.0f, 1.0f };
	constexpr Range kExposure{ 0.0f, 4.0f };
	constexpr Range kGamma{ 0.2f, 2.0f };
	constexpr Range kShadowLift{ 0.0f, 0.25f };
	constexpr Range kGrain{ 0.0f, 0.1f };
	constexpr Range kTransition{ 0.0f, 2.0f };
	constexpr Range kEyeshine{ 0.0f, 8.0f };
	constexpr Range kSeparation{ -32.0f, 32.0f };
	constexpr Range kActorBrightness{ 0.0f, 4.0f };
	constexpr int kMaximumVisionMode = 2;

	constexpr float kQueryInterval = 0.1f;
	constexpr float kMaximumFrameStep = 0.1f;
	constexpr std::uint32_t kMaximumActorHandles = 4096;
	constexpr std::uint32_t kEnemyAffiliation = 1;
	constexpr std::uint32_t kAllyAffiliation = 2;

	constexpr UINT kEyeshineMaskSlot = 124;
	constexpr std::size_t kMaximumEyeMasks = 64;
	constexpr UINT kMaximumMaskDimension = 2048;
	constexpr auto kMaskDirectory = "Data\\Shaders\\NightEye\\Eyeshine";
	constexpr auto kShaderPath = L"Data\\Shaders\\NightEye\\NightEye.hlsl";

	using ExtraFlags = State::ExtraShaderDescriptors;
	constexpr auto kEyeshineFlag = static_cast<std::uint32_t>(ExtraFlags::NightEyeEyeshine);
	constexpr auto kMaskFlag = static_cast<std::uint32_t>(ExtraFlags::NightEyeMask);
	constexpr auto kActorFlag = static_cast<std::uint32_t>(ExtraFlags::NightEyeActor);
	constexpr auto kEnemyFlag = static_cast<std::uint32_t>(ExtraFlags::NightEyeEnemy);
	constexpr auto kAllyFlag = static_cast<std::uint32_t>(ExtraFlags::NightEyeAlly);
	constexpr auto kAllFlags = kEyeshineFlag | kMaskFlag | kActorFlag | kEnemyFlag | kAllyFlag;

	struct alignas(16) RenderData
	{
		float4 ExposureGammaLiftStrength;
		float4 Tint;
		float4 VFX;
		float4 BufferDim;
		float4 Anaglyph;
		float4 AnaglyphLeft;
		float4 AnaglyphRight;
		float4 HDRData;
	};
	static_assert(sizeof(RenderData) == 128);

	struct ScalarRange
	{
		float NightEye::Settings::* field;
		Range range;
	};
	constexpr std::array kScalarRanges{
		ScalarRange{ &NightEye::Settings::Exposure, kExposure },
		ScalarRange{ &NightEye::Settings::Gamma, kGamma },
		ScalarRange{ &NightEye::Settings::ShadowLift, kShadowLift },
		ScalarRange{ &NightEye::Settings::TintAmount, kUnit },
		ScalarRange{ &NightEye::Settings::Glow, kUnit },
		ScalarRange{ &NightEye::Settings::Vignette, kUnit },
		ScalarRange{ &NightEye::Settings::Grain, kGrain },
		ScalarRange{ &NightEye::Settings::TransitionSeconds, kTransition },
		ScalarRange{ &NightEye::Settings::EyeshineAmount, kEyeshine },
		ScalarRange{ &NightEye::Settings::CustomAmount, kUnit },
		ScalarRange{ &NightEye::Settings::AnaglyphAmount, kUnit },
		ScalarRange{ &NightEye::Settings::AnaglyphHorizontal, kSeparation },
		ScalarRange{ &NightEye::Settings::AnaglyphVertical, kSeparation },
		ScalarRange{ &NightEye::Settings::AnaglyphDesaturation, kUnit },
		ScalarRange{ &NightEye::Settings::ActorVisionAmount, kUnit },
		ScalarRange{ &NightEye::Settings::ActorVisionBrightness, kActorBrightness }
	};

	void Sanitize(NightEye::Settings& a_settings)
	{
		const NightEye::Settings defaults;
		for (const auto& entry : kScalarRanges) {
			auto& value = a_settings.*entry.field;
			value = std::isfinite(value) ? std::clamp(value, entry.range.minimum, entry.range.maximum) : defaults.*entry.field;
		}
		a_settings.VisionMode = std::clamp(a_settings.VisionMode, 0, kMaximumVisionMode);
		for (auto field : { &NightEye::Settings::TintColor, &NightEye::Settings::EyeshineColor, &NightEye::Settings::AnaglyphLeftColor,
				 &NightEye::Settings::AnaglyphRightColor, &NightEye::Settings::EnemyColor, &NightEye::Settings::AllyColor, &NightEye::Settings::NeutralColor }) {
			auto& color = a_settings.*field;
			for (std::size_t component = 0; component < color.size(); ++component)
				color[component] = std::isfinite(color[component]) ? std::clamp(color[component], kUnit.minimum, kUnit.maximum) : (defaults.*field)[component];
		}
	}

	float4 ToFloat4(const std::array<float, 3>& a_color, float a_w)
	{
		return { a_color[0], a_color[1], a_color[2], a_w };
	}

	class EffectVisitor : public RE::MagicTarget::ForEachActiveEffectVisitor
	{
	public:
		explicit EffectVisitor(const std::vector<const RE::EffectSetting*>& a_effects) :
			effects(a_effects) {}

		RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* a_effect) override
		{
			if (a_effect && !a_effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
				const auto* base = a_effect->GetBaseObject();
				if (base && (std::ranges::find(effects, base) != effects.end() || base->HasArchetype(RE::EffectSetting::Archetype::kNightEye))) {
					found = true;
					return RE::BSContainer::ForEachResult::kStop;
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		}

		bool found = false;

	private:
		const std::vector<const RE::EffectSetting*>& effects;
	};

	template <class Shader>
	struct ShaderState
	{
		Shader* shader = nullptr;
		std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};
		UINT count = D3D11_SHADER_MAX_INTERFACES;

		~ShaderState()
		{
			if (shader)
				shader->Release();
			for (auto* instance : instances)
				if (instance)
					instance->Release();
		}
	};

	class PostProcessState
	{
	public:
		explicit PostProcessState(ID3D11DeviceContext* a_context) :
			context(a_context)
		{
			context->VSGetShader(&vs.shader, vs.instances.data(), &vs.count);
			context->PSGetShader(&ps.shader, ps.instances.data(), &ps.count);
			context->GSGetShader(&gs.shader, gs.instances.data(), &gs.count);
			context->HSGetShader(&hs.shader, hs.instances.data(), &hs.count);
			context->DSGetShader(&ds.shader, ds.instances.data(), &ds.count);
			context->IAGetInputLayout(layout.put());
			context->IAGetPrimitiveTopology(&topology);
			context->PSGetConstantBuffers(0, 1, buffer.put());
			context->PSGetShaderResources(0, 1, resource.put());
			context->PSGetSamplers(0, 1, sampler.put());
			context->RSGetState(rasterizer.put());
			context->RSGetViewports(&viewportCount, viewports.data());
			context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), depth.put());
			context->OMGetBlendState(blend.put(), blendFactor.data(), &sampleMask);
			context->OMGetDepthStencilState(depthState.put(), &stencil);
		}

		~PostProcessState()
		{
			ID3D11ShaderResourceView* noResource = nullptr;
			context->PSSetShaderResources(0, 1, &noResource);
			context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), depth.get());
			context->OMSetBlendState(blend.get(), blendFactor.data(), sampleMask);
			context->OMSetDepthStencilState(depthState.get(), stencil);
			context->RSSetState(rasterizer.get());
			context->RSSetViewports(viewportCount, viewports.data());
			context->IASetInputLayout(layout.get());
			context->IASetPrimitiveTopology(topology);
			context->VSSetShader(vs.shader, vs.instances.data(), vs.count);
			context->PSSetShader(ps.shader, ps.instances.data(), ps.count);
			context->GSSetShader(gs.shader, gs.instances.data(), gs.count);
			context->HSSetShader(hs.shader, hs.instances.data(), hs.count);
			context->DSSetShader(ds.shader, ds.instances.data(), ds.count);
			auto* savedBuffer = buffer.get();
			auto* savedResource = resource.get();
			auto* savedSampler = sampler.get();
			context->PSSetConstantBuffers(0, 1, &savedBuffer);
			context->PSSetShaderResources(0, 1, &savedResource);
			context->PSSetSamplers(0, 1, &savedSampler);
			for (auto* target : targets)
				if (target)
					target->Release();
		}

		PostProcessState(const PostProcessState&) = delete;
		PostProcessState& operator=(const PostProcessState&) = delete;

	private:
		ID3D11DeviceContext* context;
		ShaderState<ID3D11VertexShader> vs;
		ShaderState<ID3D11PixelShader> ps;
		ShaderState<ID3D11GeometryShader> gs;
		ShaderState<ID3D11HullShader> hs;
		ShaderState<ID3D11DomainShader> ds;
		winrt::com_ptr<ID3D11InputLayout> layout;
		D3D11_PRIMITIVE_TOPOLOGY topology{};
		winrt::com_ptr<ID3D11Buffer> buffer;
		winrt::com_ptr<ID3D11ShaderResourceView> resource;
		winrt::com_ptr<ID3D11SamplerState> sampler;
		winrt::com_ptr<ID3D11RasterizerState> rasterizer;
		std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
		UINT viewportCount = static_cast<UINT>(viewports.size());
		std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
		winrt::com_ptr<ID3D11DepthStencilView> depth;
		winrt::com_ptr<ID3D11BlendState> blend;
		std::array<float, 4> blendFactor{};
		UINT sampleMask = 0;
		winrt::com_ptr<ID3D11DepthStencilState> depthState;
		UINT stencil = 0;
	};

	constexpr auto kEyeWords = std::to_array<std::string_view>({ "eye", "eyes", "eyeball", "eyeballs", "eyegeo", "eyesgeo", "eyemesh", "eyesmesh",
		"maleeyes", "femaleeyes", "eyesmale", "eyesfemale", "facegeneyes" });
	constexpr auto kNonEyeWords = std::to_array<std::string_view>({ "brow", "brows", "lash", "lashes", "lid", "lids", "body", "torso", "head",
		"teeth", "tooth", "socket", "sockets" });

	bool StartsNameWord(std::string_view a_name, std::size_t a_index)
	{
		const auto current = static_cast<unsigned char>(a_name[a_index]);
		const auto previous = static_cast<unsigned char>(a_name[a_index - 1]);
		if (std::isdigit(current) != std::isdigit(previous))
			return true;
		const bool acronymEnd = std::isupper(previous) && a_index + 1 < a_name.size() && std::islower(static_cast<unsigned char>(a_name[a_index + 1]));
		return std::isupper(current) && (std::islower(previous) || acronymEnd);
	}

	bool IsEyeName(std::string_view a_name)
	{
		bool hasEyeWord = false;
		for (std::size_t begin = 0; begin < a_name.size();) {
			if (!std::isalnum(static_cast<unsigned char>(a_name[begin]))) {
				++begin;
				continue;
			}
			std::size_t end = begin + 1;
			while (end < a_name.size() && std::isalnum(static_cast<unsigned char>(a_name[end])) && !StartsNameWord(a_name, end))
				++end;
			std::string word(a_name.substr(begin, end - begin));
			std::ranges::transform(word, word.begin(), [](unsigned char a_character) { return static_cast<char>(std::tolower(a_character)); });
			if (std::ranges::find(kNonEyeWords, word) != kNonEyeWords.end())
				return false;
			hasEyeWord |= std::ranges::find(kEyeWords, word) != kEyeWords.end();
			begin = end;
		}
		return hasEyeWord;
	}

	std::optional<bool> GetEyeshineOverride(RE::BSGeometry* a_geometry)
	{
		if (!a_geometry)
			return std::nullopt;
		static const RE::BSFixedString eyeshineTag = "NightEyeEyeshine";
		if (auto* extraData = a_geometry->GetExtraData(eyeshineTag)) {
			const auto* integer = netimmerse_cast<RE::NiIntegerExtraData*>(extraData);
			return integer && integer->value > 0;
		}
		return std::nullopt;
	}

	std::string NormalizeTexturePath(std::string a_path)
	{
		for (auto& character : a_path)
			character = character == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		if (a_path.starts_with("data\\"))
			a_path.erase(0, 5);
		if (!a_path.starts_with("textures\\"))
			a_path.insert(0, "textures\\");
		return a_path;
	}

	// Installed after Camera FX's detour, so it runs first and lens effects land on the graded scene.
	struct MenuManager_DrawInterfaceStart
	{
		static void thunk(int64_t a1)
		{
			globals::features::nightEye.DrawBeforeInterface();
			func(a1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Marks frames where the world was drawn; freeze-frame menus reuse the old image, which must not be graded again.
	struct Main_PostProcessing
	{
		static void thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5)
		{
			func(a_this, a3, a_target, a_4, a_5);
			if (a_target == RE::RENDER_TARGET::kFRAMEBUFFER)
				globals::features::nightEye.MarkWorldPostProcessed();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void NightEye::SetupResources()
{
	auto* device = globals::d3d::device;

	parameters = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<RenderData>(), "NightEye::PostProcessParameters");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	if (SUCCEEDED(device->CreateSamplerState(&samplerDesc, sampler.put())))
		Util::SetResourceName(sampler.get(), "NightEye::Sampler");

	D3D11_BLEND_DESC blendDesc{};
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (SUCCEEDED(device->CreateBlendState(&blendDesc, blendState.put())))
		Util::SetResourceName(blendState.get(), "NightEye::BlendState");

	D3D11_RASTERIZER_DESC rasterizerDesc{};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.DepthClipEnable = true;
	if (SUCCEEDED(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put())))
		Util::SetResourceName(rasterizerState.get(), "NightEye::Rasterizer");

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
	if (SUCCEEDED(device->CreateDepthStencilState(&depthDesc, depthStencilState.put())))
		Util::SetResourceName(depthStencilState.get(), "NightEye::DepthState");

	if (!sampler || !blendState || !rasterizerState || !depthStencilState)
		logger::error("[NightEye] Failed to create post-process states");

	PrepareShaders();
}

bool NightEye::PrepareShaders()
{
	if (!reloadShaders)
		return vertexShader && pixelShader;
	reloadShaders = false;

	winrt::com_ptr<ID3D11VertexShader> newVertex;
	winrt::com_ptr<ID3D11PixelShader> newPixel;
	newVertex.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(kShaderPath, { { "VSHADER", "1" } }, "vs_5_0", "mainVS")));
	newPixel.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(kShaderPath, { { "PSHADER", "1" } }, "ps_5_0", "mainPS")));
	if (!newVertex || !newPixel) {
		logger::error("[NightEye] Night Eye shader compilation failed; the previous shader is kept when available");
		shaderCompileFailed = true;
		postProcessReady.store(vertexShader && pixelShader && sampler && blendState && rasterizerState && depthStencilState, std::memory_order_release);
		return vertexShader && pixelShader;
	}

	Util::SetResourceName(newVertex.get(), "NightEye::FullscreenVS");
	Util::SetResourceName(newPixel.get(), "NightEye::PostProcessPS");
	vertexShader = std::move(newVertex);
	pixelShader = std::move(newPixel);
	shaderCompileFailed = false;
	postProcessReady.store(sampler && blendState && rasterizerState && depthStencilState, std::memory_order_release);
	return true;
}

void NightEye::ClearShaderCache()
{
	reloadShaders = true;
}

bool NightEye::PrepareInput(ID3D11Texture2D* a_output, ID3D11RenderTargetView* a_view)
{
	D3D11_TEXTURE2D_DESC description{};
	D3D11_RENDER_TARGET_VIEW_DESC viewDescription{};
	a_output->GetDesc(&description);
	a_view->GetDesc(&viewDescription);
	if (description.SampleDesc.Count != 1 || description.ArraySize != 1 || viewDescription.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || viewDescription.Texture2D.MipSlice != 0)
		return false;
	if (std::memcmp(&description, &inputDescription, sizeof(description)) == 0 && inputFormat == viewDescription.Format)
		return inputTexture && inputView;

	auto* device = globals::d3d::device;
	auto copyDescription = description;
	copyDescription.Usage = D3D11_USAGE_DEFAULT;
	copyDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	copyDescription.CPUAccessFlags = 0;
	copyDescription.MiscFlags = 0;

	winrt::com_ptr<ID3D11Texture2D> texture;
	winrt::com_ptr<ID3D11ShaderResourceView> view;
	auto result = device->CreateTexture2D(&copyDescription, nullptr, texture.put());
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDescription{};
	srvDescription.Format = viewDescription.Format;
	srvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDescription.Texture2D.MipLevels = 1;
	if (SUCCEEDED(result))
		result = device->CreateShaderResourceView(texture.get(), &srvDescription, view.put());
	if (FAILED(result)) {
		logger::error("[NightEye] Scene copy failed: 0x{:08X}, {}x{}, format {}", static_cast<unsigned int>(result), description.Width, description.Height, static_cast<unsigned int>(description.Format));
		return false;
	}

	Util::SetResourceName(texture.get(), "NightEye::SceneCopy");
	Util::SetResourceName(view.get(), "NightEye::SceneCopy SRV");
	inputDescription = description;
	inputFormat = viewDescription.Format;
	inputTexture = std::move(texture);
	inputView = std::move(view);
	return true;
}

void NightEye::DataLoaded()
{
	effects.clear();
	imageSpaceModifiers.clear();
	visionModifiers.clear();
	if (auto* handler = RE::TESDataHandler::GetSingleton()) {
		struct EffectForm
		{
			RE::FormID localId;
			std::string_view plugin;
		};
		constexpr std::array forms{ EffectForm{ 0x6B10C, "Skyrim.esm" },
			EffectForm{ 0x803, "ccBGSSSE037-curios.esl" },
			EffectForm{ 0xBC4F8, "Ordinator - Perks of Skyrim.esp" } };
		for (const auto& form : forms) {
			if (auto* effect = handler->LookupForm<RE::EffectSetting>(form.localId, form.plugin))
				effects.push_back(effect);
		}

		struct ModifierForm
		{
			RE::FormID localId;
			std::string_view plugin;
			bool vision;
		};
		constexpr std::array modifierForms{ ModifierForm{ 0x6B108, "Skyrim.esm", true },
			ModifierForm{ 0x6B10A, "Skyrim.esm", true },
			ModifierForm{ 0x6B10B, "Skyrim.esm", false },
			ModifierForm{ 0x2C242C, "GodsAndWorship.esp", true } };
		for (const auto& form : modifierForms) {
			if (auto* modifier = handler->LookupForm<RE::TESImageSpaceModifier>(form.localId, form.plugin)) {
				imageSpaceModifiers.push_back(modifier);
				if (form.vision)
					visionModifiers.push_back(modifier);
			}
		}
	}
	logger::info("[NightEye] Resolved {} listed Night Eye magic effects and {} imagespace modifiers", effects.size(), imageSpaceModifiers.size());

	LoadEyeshineMaskTable();
	queryElapsed = kQueryInterval;
}

bool NightEye::ReplacesImageSpaceModifier(const RE::TESImageSpaceModifier* a_modifier) const
{
	return loaded && settings.Enabled && postProcessReady.load(std::memory_order_acquire) && a_modifier &&
	       std::ranges::find(imageSpaceModifiers, a_modifier) != imageSpaceModifiers.end();
}

bool NightEye::IsVisionModifier(const RE::TESImageSpaceModifier* a_modifier) const
{
	return a_modifier && std::ranges::find(visionModifiers, a_modifier) != visionModifiers.end();
}

void NightEye::PostPostLoad()
{
	ImageSpaceModifiers::Install();
	if (!REL::Module::IsVR()) {
		stl::write_thunk_call<Main_PostProcessing>(REL::RelocationID(100430, 107148).address() + REL::Relocate(0x1F0, 0x1E7));
		stl::detour_thunk<MenuManager_DrawInterfaceStart>(REL::RelocationID(79947, 82084));
		logger::info("[NightEye] Hooked world post-processing and interface start");
	}
}

void NightEye::MarkWorldPostProcessed()
{
	if (auto* state = globals::state) {
		worldPostProcessedFrame = state->frameCount;
		worldPostProcessSeen = true;
	}
}

void NightEye::DrawBeforeInterface()
{
	auto* state = globals::state;
	if (!loaded || !state || !globals::game::renderer || activation <= 0.0f)
		return;
	if (lastDrawnFrame == state->frameCount)
		return;
	// No fresh world this frame (a freeze-frame menu is open): grading again would compound on last frame's output.
	if (worldPostProcessSeen && worldPostProcessedFrame != state->frameCount)
		return;
	lastDrawnFrame = state->frameCount;

	const auto target = globals::features::cameraFX.GetSceneTarget();
	if (!target.texture || !target.rtv)
		return;
	RenderPostProcess(target.texture, target.rtv);
}

void NightEye::GameLoaded()
{
	active.store(false, std::memory_order_release);
	affiliations.store(nullptr, std::memory_order_release);
}

void NightEye::QueryGame(bool a_actorVision)
{
	EffectVisitor visitor{ effects };
	bool found = false;
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (player && player->GetParentCell() && !globals::state->IsMainOrLoadingMenuOpen()) {
		if (auto* target = player->GetMagicTarget())
			target->VisitEffects(visitor);
		found = visitor.found || ImageSpaceModifiers::IsNightEyeActive();
	}

	std::shared_ptr<const Affiliations> snapshot;
	if (found && a_actorVision) {
		if (auto* lists = RE::ProcessLists::GetSingleton()) {
			auto values = std::make_shared<Affiliations>();
			const auto count = std::min<std::uint32_t>(lists->highActorHandles.size(), kMaximumActorHandles);
			values->reserve(count);
			for (std::uint32_t index = 0; index < count; ++index) {
				auto actor = lists->highActorHandles[index].get();
				if (!actor || actor.get() == player || !actor->Is3DLoaded() || actor->IsDead())
					continue;
				std::uint32_t affiliation = 0;
				if (actor->IsHostileToActor(player))
					affiliation = kEnemyAffiliation;
				else if (actor->IsPlayerTeammate() || actor->IsSummonedByPlayer() || actor->GetFactionReaction(player) == RE::FIGHT_REACTION::kAlly)
					affiliation = kAllyAffiliation;
				values->emplace(actor->GetFormID(), affiliation);
			}
			snapshot = std::move(values);
		}
	}

	affiliations.store(std::move(snapshot), std::memory_order_release);
	active.store(found, std::memory_order_release);
	queryPending.store(false, std::memory_order_release);
}

void NightEye::Reset()
{
	boundMaskValid = false;

	const float frameDelta = RE::GetSecondsSinceLastFrame();
	const float delta = std::isfinite(frameDelta) ? std::clamp(frameDelta, 0.0f, kMaximumFrameStep) : 0.0f;
	auto* state = globals::state;
	const bool blocked = state->IsMainOrLoadingMenuOpen() || state->isMapMenuOpen || state->isStatsMenuOpen;
	if (!settings.Enabled || blocked) {
		activation = 0.0f;
		frameAffiliations.reset();
		queryElapsed = kQueryInterval;
		return;
	}

	elapsed += delta;
	queryElapsed += delta;
	if (queryElapsed >= kQueryInterval) {
		if (auto* tasks = SKSE::GetTaskInterface(); tasks && !queryPending.exchange(true)) {
			queryElapsed = 0.0f;
			const bool actorVision = settings.VisionMode != 0 && settings.ActorVisionAmount > 0.0f;
			tasks->AddTask([this, actorVision] { QueryGame(actorVision); });
		}
	}

	const float target = active.load(std::memory_order_acquire) ? 1.0f : 0.0f;
	const float step = settings.TransitionSeconds > 0.0f ? delta / settings.TransitionSeconds : 1.0f;
	activation += std::clamp(target - activation, -step, step);

	auto snapshot = affiliations.load(std::memory_order_acquire);
	if (snapshot || activation <= 0.0f)
		frameAffiliations = std::move(snapshot);

	if (activation > 0.0f && settings.EyeshineAmount > 0.0f)
		LoadEyeshineMaskTextures();
}

NightEye::GeometrySettings NightEye::GetCommonBufferData() const
{
	if (!loaded)
		return {};
	return {
		ToFloat4(settings.EyeshineColor, activation * settings.EyeshineAmount),
		{ static_cast<float>(settings.VisionMode), activation * settings.ActorVisionAmount, settings.ActorVisionBrightness, 0.0f },
		ToFloat4(settings.EnemyColor, 0.0f),
		ToFloat4(settings.AllyColor, 0.0f),
		ToFloat4(settings.NeutralColor, 0.0f)
	};
}

void NightEye::SetupGeometry(RE::BSRenderPass* a_pass)
{
	auto& descriptor = globals::state->permutationData.ExtraShaderDescriptor;
	descriptor &= ~kAllFlags;
	if (activation <= 0.0f || !a_pass || !a_pass->geometry)
		return;

	const float eyeAmount = activation * settings.EyeshineAmount;
	auto* mask = eyeAmount > 0.0f ? GetEyeshineMask(a_pass) : nullptr;
	if (mask || (eyeAmount > 0.0f && IsEyeGeometry(a_pass)))
		descriptor |= kEyeshineFlag;
	if (mask) {
		descriptor |= kMaskFlag;
		if (!boundMaskValid || boundMask != mask) {
			globals::d3d::context->PSSetShaderResources(kEyeshineMaskSlot, 1, &mask);
			boundMask = mask;
			boundMaskValid = true;
		}
	}

	if (frameAffiliations) {
		const auto* reference = a_pass->geometry->GetUserData();
		const auto* actor = reference ? reference->As<RE::Actor>() : nullptr;
		if (actor) {
			if (const auto found = frameAffiliations->find(actor->GetFormID()); found != frameAffiliations->end()) {
				descriptor |= kActorFlag;
				if (found->second == kEnemyAffiliation)
					descriptor |= kEnemyFlag;
				else if (found->second == kAllyAffiliation)
					descriptor |= kAllyFlag;
			}
		}
	}
}

void NightEye::RenderPostProcess(ID3D11Texture2D* a_texture, ID3D11RenderTargetView* a_view)
{
	if (!loaded || !parameters || !sampler || !blendState || !rasterizerState || !depthStencilState || activation <= 0.0f || !PrepareShaders())
		return;

	auto* texture = a_texture;
	auto* view = a_view;
	if (!texture || !view || !PrepareInput(texture, view))
		return;

	const float direction = settings.AnaglyphSwap ? -1.0f : 1.0f;
	const bool floatInput = inputFormat == DXGI_FORMAT_R16G16B16A16_FLOAT || inputFormat == DXGI_FORMAT_R32G32B32A32_FLOAT;
	const RenderData data{
		{ settings.Exposure, settings.Gamma, settings.ShadowLift, activation },
		ToFloat4(settings.TintColor, settings.VisionMode == 0 ? settings.TintAmount : 0.0f),
		{ settings.Glow, settings.Vignette, settings.Grain, settings.CustomVFX ? settings.CustomAmount : 0.0f },
		{ 1.0f / static_cast<float>(inputDescription.Width), 1.0f / static_cast<float>(inputDescription.Height), elapsed, floatInput ? 1.0f : 0.0f },
		{ settings.AnaglyphEnabled ? settings.AnaglyphAmount : 0.0f, direction * settings.AnaglyphHorizontal, direction * settings.AnaglyphVertical, settings.AnaglyphDesaturation },
		ToFloat4(settings.AnaglyphLeftColor, 0.0f),
		ToFloat4(settings.AnaglyphRightColor, 0.0f),
		globals::features::hdrDisplay.GetSharedDataHDR()
	};
	parameters->Update(data);

	auto* context = globals::d3d::context;
	globals::profiler->BeginPass("NightEye::PostProcess");
	{
		PostProcessState backup(context);
		context->OMSetRenderTargets(0, nullptr, nullptr);
		ID3D11ShaderResourceView* noResource = nullptr;
		context->PSSetShaderResources(0, 1, &noResource);
		context->CopyResource(inputTexture.get(), texture);

		const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(inputDescription.Width), static_cast<float>(inputDescription.Height), 0.0f, 1.0f };
		context->RSSetViewports(1, &viewport);
		context->RSSetState(rasterizerState.get());
		context->OMSetRenderTargets(1, &view, nullptr);
		context->OMSetBlendState(blendState.get(), nullptr, D3D11_DEFAULT_SAMPLE_MASK);
		context->OMSetDepthStencilState(depthStencilState.get(), 0);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->VSSetShader(vertexShader.get(), nullptr, 0);
		context->PSSetShader(pixelShader.get(), nullptr, 0);

		auto* buffer = parameters->CB();
		auto* passSampler = sampler.get();
		auto* scene = inputView.get();
		context->PSSetConstantBuffers(0, 1, &buffer);
		context->PSSetSamplers(0, 1, &passSampler);
		context->PSSetShaderResources(0, 1, &scene);
		context->Draw(3, 0);
	}
	globals::profiler->EndPass();
}

void NightEye::LoadEyeshineMaskTable()
{
	eyeshineMasks.clear();
	eyeshineMaskAliases.clear();
	eyeshineMaskCache.clear();
	eyeshineMasksLoaded = false;

	const auto configurationPath = std::filesystem::path(kMaskDirectory) / "Masks.json";
	std::ifstream stream(configurationPath);
	if (!stream.is_open()) {
		logger::warn("[NightEye] Eyeshine mask configuration unavailable: {}", configurationPath.string());
		return;
	}

	try {
		const auto configuration = json::parse(stream);
		if (!configuration.is_array() || configuration.size() > kMaximumEyeMasks)
			throw std::runtime_error("Eyeshine mask configuration must be an array with at most 64 entries");
		eyeshineMasks.reserve(configuration.size());
		for (const auto& entry : configuration) {
			const auto maskFile = std::filesystem::path(entry.at("mask").get<std::string>());
			if (maskFile.empty() || maskFile.has_parent_path() || maskFile.extension() != ".dds")
				throw std::runtime_error("Eyeshine masks must name a DDS file inside the Eyeshine directory");
			EyeshineMask mask;
			mask.filename = maskFile.string();
			for (const auto& name : entry.at("geometry"))
				mask.geometryNames.emplace_back(name.get<std::string>());
			if (mask.geometryNames.empty())
				throw std::runtime_error("Eyeshine masks require explicit geometry names");
			const auto index = eyeshineMasks.size();
			eyeshineMasks.push_back(std::move(mask));
			for (const auto& alias : entry.at("diffuse"))
				eyeshineMaskAliases.insert_or_assign(NormalizeTexturePath(alias.get<std::string>()), index);
		}
		logger::info("[NightEye] Read {} eyeshine mask entries", eyeshineMasks.size());
	} catch (const std::exception& e) {
		logger::error("[NightEye] Failed to load {}: {}", configurationPath.string(), e.what());
		eyeshineMasks.clear();
		eyeshineMaskAliases.clear();
	}
}

void NightEye::LoadEyeshineMaskTextures()
{
	if (eyeshineMasksLoaded)
		return;
	eyeshineMasksLoaded = true;

	auto* device = globals::d3d::device;
	std::size_t loadedCount = 0;
	for (auto& mask : eyeshineMasks) {
		const auto maskPath = std::filesystem::path(kMaskDirectory) / mask.filename;
		winrt::com_ptr<ID3D11Resource> resource;
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		if (FAILED(DirectX::CreateDDSTextureFromFile(device, maskPath.c_str(), resource.put(), view.put()))) {
			logger::warn("[NightEye] Unable to load eyeshine mask {}", maskPath.string());
			continue;
		}
		winrt::com_ptr<ID3D11Texture2D> texture;
		D3D11_TEXTURE2D_DESC description{};
		if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
			texture->GetDesc(&description);
		if (!texture || description.Format != DXGI_FORMAT_R8_UNORM || description.ArraySize != 1 || description.SampleDesc.Count != 1 ||
			description.Width > kMaximumMaskDimension || description.Height > kMaximumMaskDimension) {
			logger::warn("[NightEye] Unsupported eyeshine mask format or dimensions in {}", maskPath.string());
			continue;
		}
		Util::SetResourceName(texture.get(), "NightEye::EyeshineMask[%s]", mask.filename.c_str());
		Util::SetResourceName(view.get(), "NightEye::EyeshineMask[%s] SRV", mask.filename.c_str());
		mask.view = std::move(view);
		++loadedCount;
	}
	logger::info("[NightEye] Loaded {} of {} eyeshine masks", loadedCount, eyeshineMasks.size());
}

bool NightEye::IsEyeGeometry(RE::BSRenderPass* a_pass)
{
	auto* geometry = a_pass->geometry;
	auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
	if (!property)
		return false;
	auto* lightingProperty = netimmerse_cast<RE::BSLightingShaderProperty*>(property);
	if (!lightingProperty && !netimmerse_cast<RE::BSEffectShaderProperty*>(property))
		return false;

	if (const auto override = GetEyeshineOverride(geometry); override.has_value())
		return *override;
	if (lightingProperty && (lightingProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kEyeReflect) ||
								(lightingProperty->material && lightingProperty->material->GetFeature() == RE::BSShaderMaterial::Feature::kEye)))
		return true;

	const auto& name = geometry->name;
	if (name.empty())
		return false;
	auto [entry, inserted] = eyeNames.try_emplace(name, false);
	if (inserted) {
		entry->second = IsEyeName(std::string_view(name.c_str(), name.length()));
		if (entry->second)
			logger::info("[NightEye] Recognized {} eye geometry {}", lightingProperty ? "lighting" : "effect", name.c_str());
	}
	return entry->second;
}

ID3D11ShaderResourceView* NightEye::GetEyeshineMask(RE::BSRenderPass* a_pass)
{
	if (!a_pass->shaderProperty || eyeshineMasks.empty())
		return nullptr;
	if (const auto override = GetEyeshineOverride(a_pass->geometry); override && !*override)
		return nullptr;
	const auto* shaderMaterial = a_pass->shaderProperty->material;
	if (!shaderMaterial || shaderMaterial->GetType() != RE::BSShaderMaterial::Type::kLighting)
		return nullptr;
	const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(shaderMaterial);
	if (!material->diffuseTexture)
		return nullptr;
	const auto& name = material->diffuseTexture->name;
	if (name.empty())
		return nullptr;

	auto found = eyeshineMaskCache.find(name);
	if (found == eyeshineMaskCache.end()) {
		const auto alias = eyeshineMaskAliases.find(NormalizeTexturePath(name.c_str()));
		const auto index = alias == eyeshineMaskAliases.end() ? std::numeric_limits<std::size_t>::max() : alias->second;
		found = eyeshineMaskCache.emplace(name, index).first;
	}
	if (found->second >= eyeshineMasks.size())
		return nullptr;
	auto& mask = eyeshineMasks[found->second];
	if (!mask.view || std::ranges::find(mask.geometryNames, a_pass->geometry->name) == mask.geometryNames.end())
		return nullptr;
	if (!mask.matchLogged) {
		logger::info("[NightEye] Eyeshine mask {} matches geometry {}, texture {}", mask.filename, a_pass->geometry->name.c_str(), name.c_str());
		mask.matchLogged = true;
	}
	return mask.view.get();
}

void NightEye::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("enable"), "Enable Night Eye"), &settings.Enabled);
	ImGui::TextUnformatted(IsEffectActive() ? T(TKEY("active"), "Night Eye is active") : T(TKEY("inactive"), "Night Eye is inactive"));

	if (!ImGui::BeginTabBar("##NightEyeTabs"))
		return;

	if (ImGui::BeginTabItem(T(TKEY("tab_visibility"), "Visibility"))) {
		ImGui::SliderFloat(T(TKEY("exposure"), "Exposure (stops)"), &settings.Exposure, kExposure.minimum, kExposure.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("gamma"), "Shadow Gamma"), &settings.Gamma, kGamma.minimum, kGamma.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("shadow_lift"), "Shadow Lift"), &settings.ShadowLift, kShadowLift.minimum, kShadowLift.maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("transition"), "Transition (seconds)"), &settings.TransitionSeconds, kTransition.minimum, kTransition.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::ColorEdit3(T(TKEY("tint_color"), "Tint Color"), settings.TintColor.data());
		ImGui::SliderFloat(T(TKEY("tint_amount"), "Tint Amount"), &settings.TintAmount, kUnit.minimum, kUnit.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("glow"), "Glow"), &settings.Glow, kUnit.minimum, kUnit.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("vignette"), "Vignette"), &settings.Vignette, kUnit.minimum, kUnit.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("grain"), "Grain"), &settings.Grain, kGrain.minimum, kGrain.maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_eyes"), "Eyeshine"))) {
		ImGui::SliderFloat(T(TKEY("eyeshine_amount"), "Eyeshine Strength"), &settings.EyeshineAmount, kEyeshine.minimum, kEyeshine.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::ColorEdit3(T(TKEY("eyeshine_color"), "Eyeshine Color"), settings.EyeshineColor.data());
		ImGui::TextWrapped("%s", T(TKEY("eye_compatibility"), "Affects human and creature eye meshes, plus supported creature body atlases. Custom combined meshes need an eyeshine mask."));
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_anaglyph"), "Anaglyph"))) {
		ImGui::Checkbox(T(TKEY("anaglyph_enable"), "Enable Colour Split"), &settings.AnaglyphEnabled);
		ImGui::SliderFloat(T(TKEY("anaglyph_amount"), "Colour Split Amount"), &settings.AnaglyphAmount, kUnit.minimum, kUnit.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("anaglyph_horizontal"), "Horizontal Separation (pixels)"), &settings.AnaglyphHorizontal, kSeparation.minimum, kSeparation.maximum, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("anaglyph_vertical"), "Vertical Separation (pixels)"), &settings.AnaglyphVertical, kSeparation.minimum, kSeparation.maximum, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("anaglyph_desaturation"), "Colour Split Desaturation"), &settings.AnaglyphDesaturation, kUnit.minimum, kUnit.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::ColorEdit3(T(TKEY("anaglyph_left"), "Left Channel Colour"), settings.AnaglyphLeftColor.data());
		ImGui::ColorEdit3(T(TKEY("anaglyph_right"), "Right Channel Colour"), settings.AnaglyphRightColor.data());
		ImGui::Checkbox(T(TKEY("anaglyph_swap"), "Swap Channels"), &settings.AnaglyphSwap);
		ImGui::TextWrapped("%s", T(TKEY("anaglyph_description"), "Splits one camera image into two adjustable colour channels. Separation is measured at the final output resolution."));
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_actors"), "Actor Vision"))) {
		const std::array modes{ T(TKEY("vision_off"), "Off"), T(TKEY("vision_infrared"), "Infrared Style"), T(TKEY("vision_predator"), "Predator") };
		ImGui::Combo(T(TKEY("vision_mode"), "Vision Mode"), &settings.VisionMode, modes.data(), static_cast<int>(modes.size()));
		ImGui::SliderFloat(T(TKEY("actor_amount"), "Actor Highlight Amount"), &settings.ActorVisionAmount, kUnit.minimum, kUnit.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("actor_brightness"), "Actor Highlight Brightness"), &settings.ActorVisionBrightness, kActorBrightness.minimum, kActorBrightness.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::ColorEdit3(T(TKEY("enemy_color"), "Enemy Colour"), settings.EnemyColor.data());
		ImGui::ColorEdit3(T(TKEY("ally_color"), "Ally Colour"), settings.AllyColor.data());
		ImGui::ColorEdit3(T(TKEY("neutral_color"), "Neutral Colour"), settings.NeutralColor.data());
		ImGui::TextWrapped("%s", T(TKEY("vision_description"), "Highlights visible living actors using their game relationships, including modded creatures. Predator uses the editable relationship colours; Infrared Style uses simulated heat colours. Global tint is bypassed while actor vision is enabled."));
		ImGui::EndTabItem();
	}

	if (ImGui::BeginTabItem(T(TKEY("tab_custom"), "Custom VFX"))) {
		ImGui::Checkbox(T(TKEY("custom_enable"), "Enable Custom VFX"), &settings.CustomVFX);
		ImGui::SliderFloat(T(TKEY("custom_amount"), "Custom VFX Amount"), &settings.CustomAmount, kUnit.minimum, kUnit.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::TextWrapped("%s", T(TKEY("custom_file"), "Edit Data/Shaders/NightEye/CustomVFX.hlsli, then reload the shader."));
		if (ImGui::Button(T(TKEY("reload_shader"), "Reload Night Eye Shader")))
			reloadShaders = true;
		if (shaderCompileFailed)
			ImGui::TextWrapped("%s", T(TKEY("shader_failed"), "The shader did not compile. Correct the file and reload. The previous shader is retained when available. Check the log for details."));
		else if (pixelShader)
			ImGui::TextWrapped("%s", T(TKEY("shader_ready"), "Night Eye shader is ready."));
		ImGui::EndTabItem();
	}

	ImGui::EndTabBar();
}

void NightEye::SaveSettings(json& o_json)
{
	o_json = settings;
}

void NightEye::LoadSettings(json& o_json)
{
	settings = o_json;
	Sanitize(settings);
}

void NightEye::RestoreDefaultSettings()
{
	settings = {};
}

#undef I18N_KEY_PREFIX
